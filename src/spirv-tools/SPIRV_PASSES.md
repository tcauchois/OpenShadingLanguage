# The SPIR-V lowering passes

Before LLVM's SPIR-V backend emits a shader group, the shading system
rewrites the group's entry function (`eval_osl`, with everything inlined
into it). It uses a few LLVM passes of its own, in
`src/liboslexec/spirv_passes.{h,cpp}`, plus some of LLVM's. This document
covers each pass:

- why we need it;
- what it mirrors in other pipelines (LLVM's own passes, other GPU
  backends);
- whether it works around an entry in
  [TOOLCHAIN_ISSUES.md](TOOLCHAIN_ISSUES.md), and which reproducer in
  `toolchain_repros/llvm/` shows the problem.

In short: most of these passes do, earlier and more narrowly, a job the
SPIR-V backend tries to do itself but fails at for OSL's code. If the
backend bugs are fixed, several of the passes can go away. The rest are
there because of Vulkan's rules, or because of OSL's ABI.

| Pass | Why | Mirrors | Toolchain issue |
|---|---|---|---|
| `SPIRVFoldNullComparesPass` | Pointer-vs-null compares crash the backend | InstCombine's `icmp ptr, null` fold | Yes: `null_compare.ll` |
| `SPIRVWidenAddressPhisPass` | 32-bit counters in address computations crash the backend | IndVarSimplify's IV widening | Yes (partial): `loop_table_*.ll` |
| `SPIRVLowerUnorderedComparesPass` | `OpUnordered`/`OpOrdered` aren't allowed in Vulkan | Legalizers' `SETUO` → `x != x` expansion | Yes: `fcmp_uno.ll` |
| `SPIRVLowerCopysignPass` | `G_FCOPYSIGN` can't be selected | GlobalISel `LegalizerHelper::lowerFCopySign` | Yes: `copysign.ll` |
| `SPIRVLocalizeGlobalsPass` (+ SROA, InstCombine, SimplifyCFG) | Per-invocation state in registers; split `i1` globals crash the backend | GlobalOpt's localization of globals | Partly (not isolated) |
| `SPIRVRetypePointersPass` | Logical addressing needs typed access chains; the backend's own retyping crashes | SPIR-V backend's `SPIRVEmitIntrinsics` + `SPIRVLegalizePointerCast`; DirectX legalization; SROA's select speculation | Yes: `untyped_gep.ll`, `select_ptr.ll`, and others |
| `SPIRVFoldHandleCastsPass` | Pointer/integer conversions aren't allowed on logical pointers | InstCombine's `ptrtoint(inttoptr x)` fold | No (a Vulkan rule, plus cleanup of the retyping) |
| Structurization (LowerSwitch, FixIrreducible, UnifyLoopExits, StructurizeCFG) | Vulkan requires structured control flow, and the backend's structurizer gets some CFGs wrong | AMDGPU's pre-ISel pipeline | Yes: `cfg_chain.ll` |

## Where they run

The passes run after the optimizer, so they are not undone by it. Several of
them rewrite a form back into one that InstCombine would canonicalize away.
The order is:

1. `finalize_spirv` (in `llvm_spirv.cpp`) sets the entry's `byref`
   parameter attributes, which give the retyping pass the pointee types.
2. `run_spirv_early_lowering`:
   - the four lowerings (null compares, phis, unordered compares, copysign);
   - `SPIRVLocalizeGlobalsPass`;
   - if it localized anything, SROA, InstCombine and SimplifyCFG.
3. `finalize_spirv` sets module constants (the `shading_state_uniform` and
   `strict_messages` flags) and the callbacks' `byref` types.
4. `run_spirv_late_lowering`:
   - `SPIRVRetypePointersPass`, which must come after the last InstCombine
     (InstCombine writes byte-offset addresses, and would fold the all-zero
     GEPs the retyping adds);
   - `SPIRVFoldHandleCastsPass`.
5. `run_spirv_structurize`, which follows the lowerings. It's the last
   thing `finalize_spirv` does to the IR, so the module that
   `llvm_output_bitcode` dumps is the one the backend receives.
6. `LLVM_Util::spirv_compile_group` (in `llvm_util.cpp`) emits the module
   (in a child process; see *Invalid SPIR-V after a few modules* in
   TOOLCHAIN_ISSUES.md).

`add_spirv_lowering_passes()` lists steps 2 and 4 in order, without the
conditional cleanup.

The unit test `unit_spirvpasses` (`src/liboslexec/spirv_passes_test.cpp`)
runs each pass on a small function, using the reproducers' shapes where
there is one.

## SPIRVFoldNullComparesPass

**What:** folds `icmp eq/ne ptr %p, null` to a constant whenever `%p` isn't
null itself. This covers addresses of allocas and globals, including
globals `addrspacecast` to the generic address space.

**Why:** the backend crashes on pointer-vs-null compares ("SPIRV legalize
bitcast pass"). The compares come from OSL's shadeops: tests such as "was
this optional output given?" that become trivially true after inlining.
Under logical addressing, no valid pointer is null, so the fold is sound.

**Mirrors:** InstCombine/InstSimplify fold `icmp` of a known-non-null
pointer with null. They already do it for allocas. They don't for a cast
from address space 10 (Private), because the datalayout doesn't say null
is invalid there. We apply the logical-addressing rule to every address
space.

**Toolchain issue:** *Comparing a pointer with null*; repro:
`null_compare.ll`. If the backend handled these (folding, or
`OpPtrEqual`/`OpPtrNotEqual` where the operands allow), we wouldn't need
this pass.

## SPIRVWidenAddressPhisPass

**What:** widens 32-bit integer phis whose values feed GEPs (loop counters,
typically) to 64 bits, with truncations where other code uses them.

**Why:** the backend crashes on runtime-indexed loads in loops when the
index comes from a 32-bit phi.

**Mirrors:** IndVarSimplify's induction-variable widening (`WidenIV`),
which widens IVs to the pointer index width so that address computations
don't need extensions. That pass only widens phis it recognizes as
induction variables, and only when it judges it profitable. We widen
every phi that feeds an address.

**Toolchain issue:** *Runtime-indexed loads from a table in a loop*;
repros: `loop_table_index.ll`, `loop_table_vec3.ll`,
`loop_table_bytegep.ll`. This is a **partial** workaround. Loop strength
reduction seems to be the real trigger, and the tests listed there are
still `NOVULKAN`.

## SPIRVLowerUnorderedComparesPass

**What:** rewrites `fcmp uno x, y` as `(x != x) | (y != y)`, and
`fcmp ord` as its inverse, using `fcmp une`/`oeq` of each operand with
itself.

**Why:** the backend selects `OpUnordered`/`OpOrdered`, which require the
`Kernel` capability, so the module fails Vulkan validation.

**Mirrors:** the expansion targets without unordered compares get from the
SelectionDAG and GlobalISel legalizers (`SETUO` → `SETUNE x, x`). The
backend could also select `OpIsNan`, which shaders may use.

**Toolchain issue:** *Unordered/ordered float compares*; repro:
`fcmp_uno.ll`.

## SPIRVLowerCopysignPass

**What:** lowers `llvm.copysign` to integer bit operations: the magnitude's
bits without the sign bit, OR the sign operand's sign bit.

**Why:** the backend can't select `G_FCOPYSIGN`. GLSL.std.450 has no
copysign, so a lowering is needed in any case. The optimizer creates
`llvm.copysign` from OSL's `sign`-style code, which is why this runs after
optimization (and before any InstCombine: see the early-lowering order).

**Mirrors:** GlobalISel's generic `LegalizerHelper::lowerFCopySign`, which
does the same thing. The SPIR-V legalizer doesn't mark `G_FCOPYSIGN` for
lowering.

**Toolchain issue:** *`llvm.copysign` can't be selected*; repro:
`copysign.ll`. A fix in the backend would likely be small: marking
`G_FCOPYSIGN` for lowering in `SPIRVLegalizerInfo`.

## SPIRVLocalizeGlobalsPass (and its cleanup)

**What:** turns the module's mutable, internal, initialized globals into
allocas of the entry function, with their initial values stored at entry.
This covers the shadeops' message slots and message table. If it localized anything, SROA (allowed to modify the CFG),
InstCombine and SimplifyCFG follow. The cleanup splits the locals into
scalars and removes the rest. The pass leaves a global alone if any of its
uses isn't a load, a store to it, a GEP or an addrspacecast in the entry
function. For example, if its address is stored, or it's used outside the
entry.

**Why:** two reasons:

1. **Performance (OSL design):** shader-group state that lasts for one
   `eval_osl` call belongs in registers. As Private globals it would be
   memory, even after the optimizer had resolved most accesses to
   constants. SROA, the cleanup that follows, only works on allocas. This
   is why message slots end up as SSA values (see the messages discussion
   in OSL-SpirV-Design.md).
2. **Workaround:** before this pass, the optimizer split small mutable
   globals into Private globals with `i1` fields, which crashed the
   backend in OSL shader groups.

Localizing also gives the state per-call rather than per-invocation
lifetime. That is what OSL means by it, if a renderer calls `eval_osl`
more than once in an invocation.

**Mirrors:** GlobalOpt's localization: it replaces a global with an
alloca when the global is used by one non-recursive function and is dead
on entry to it. GlobalOpt only does this for single-value types, in the
alloca address space, when it can prove the value doesn't carry across
calls. We know from OSL's ABI that it doesn't, and we need the transform
for arrays and structs as well.

**Toolchain issue:** *Small mutable globals are split into Private globals
with `i1` fields*. There is no standalone repro: a Private `i1` global
alone compiles. Reason 1 would keep the pass even if that issue were
fixed.

## SPIRVRetypePointersPass

**What:** rewrites every load and store whose address isn't a typed access
into its object as one: a GEP from the object (an alloca, a global, or a
`byref` parameter) through its fields and array elements. Objects get
their types from their declarations. Parameters get theirs from `byref`
attributes. It has five steps (the `Retyper` methods):

- `expand_memops`: expands constant-size memset and memcpy into loads and
  stores of the scalar fields they cover.
- `rewrite_select_addresses`: for loads and stores through
  `gep base, (select c, k1, k2)`, loads both candidates and selects the
  value. Stores go conditionally to each candidate.
- `retype_byref_arguments`: pointers passed to declared functions (the
  renderer callbacks) become typed GEPs to the parameter's declared
  `byref` type.
- `retype_accesses`: retypes each access, finding a typed path in this
  order:
  - a constant byte offset to a field of the accessed type (the common
    case);
  - runtime indices (the optimizer's `index * stride` byte arithmetic),
    turned back into array indices;
  - a same-size scalar pun (e.g., the bits of a float field stored as an
    int): accesses the field and bitcasts;
  - a 64-bit access across two 32-bit fields, or a 32-bit access to half
    of a 64-bit field (e.g., closure handles copied as words): builds or
    extracts the halves;
  - merged stores (the optimizer combining adjacent fields' stores into
    one wide store): split back per field.

  A field holding a pointer is a 64-bit handle in OSL's SPIR-V structs, so
  it's converted with `inttoptr`/`ptrtoint` rather than a bitcast. Those
  are cleaned up by the next pass.
- `delete_dead_addresses`: removes the byte-offset GEPs left unused.

The access GEPs keep their all-zero indices (`typed_gep` doesn't fold
them), since the backend needs the explicit access of a first field.
Accesses it can't type are reported (and `finalize_spirv` fails the
group), rather than guessed at. (In the testsuite, none fail.)

**Why:** under logical addressing (`Shader` capability, without
`VariablePointers` or `PhysicalStorageBuffer`), a pointer is an access
chain into a typed variable. There's no byte arithmetic, no reinterpreting
memory as another type, and no selecting between pointers. LLVM IR has
opaque pointers, and InstCombine canonicalizes struct and array accesses
to `getelementptr i8` byte offsets. Some pass has to recover the types.

**Mirrors:** this is the job of the SPIR-V backend's own
`SPIRVEmitIntrinsics` and `SPIRVLegalizePointerCast` (the "SPIRV legalize
bitcast pass" in the crash messages). `SPIRVEmitIntrinsics` deduces and
records pointee types (`spv_assign_ptr_type`). `SPIRVLegalizePointerCast`
rewrites loads and stores through mismatched pointer types into access
chains and vector or composite extracts. Those passes handle the cases
that frontends like Clang (for HLSL) produce, but crash on many of the
optimizer's shapes. Our pass handles OSL's shapes earlier, so that the
backend sees accesses that already match.

Related elsewhere:

- The DirectX backend's legalization (DXIL also needs typed accesses)
  has, as far as we know, the same "undo InstCombine's canonicalization"
  job for `i8` GEPs and allocas. (Not checked against the LLVM 22 source:
  Homebrew's LLVM doesn't include the DirectX target.)
- `rewrite_select_addresses` is SROA's select speculation (load both
  operands of a select of pointers, then select the value), applied
  where SROA doesn't.
- `expand_memops` resembles SROA's splitting of memcpy and memset into
  per-slice accesses. The backend's alternative, `OpCopyMemorySized`,
  needs the `Addresses` capability.

**Toolchain issues:**

- *Untyped pointer arithmetic crashes the backend*: `untyped_gep.ll`
  (`typed_gep_ok.ll` is what the pass produces from it).
- *Loading the first field through the struct's own pointer*: the
  all-zero GEPs. No standalone repro.
- *Selecting or phi-ing between pointers*: `select_ptr.ll` (only selects;
  phis of pointers are avoided at the source: see `fixedIndexInverse`).
- `loop_table_bytegep.ll` is the byte-offset shape that the
  runtime-index step rewrites.

Even with a backend that never crashed, some form of this pass would be
needed for shapes the backend's legalizer doesn't attempt, such as split
64-bit handles and merged stores. A fixed `SPIRVLegalizePointerCast`
could take on more of it.

## SPIRVFoldHandleCastsPass

**What:** folds `ptrtoint (inttoptr x)` to `x`, and removes unused
`inttoptr`s.

**Why:** the retyping pass converts pointer-typed fields (64-bit handles)
with `inttoptr`/`ptrtoint`. In SPIR-V those are `OpConvertUToPtr` and
`OpConvertPtrToU`, which need `Addresses` or `PhysicalStorageBuffer`
pointers, so Vulkan rejects them on logical pointers. In OSL's code the
handles are only passed through (to callbacks, or stored back), so the
pairs cancel.

**Mirrors:** InstCombine does exactly this fold, but it can't run after
the retyping (it would undo it), so we do just this one fold.

**Toolchain issue:** none. This follows from Vulkan's rules and from OSL's
ABI (pointers as handles, see `llvm_type_sg`). It is related to *No
address space maps to `PhysicalStorageBuffer`*. With that mapping,
handles could be real buffer addresses.

## Structurization: LowerSwitch, FixIrreducible, UnifyLoopExits, StructurizeCFG

**What:** LLVM's own passes, added by `add_spirv_structurize_passes()`:

- turn switches into branches;
- make irreducible loops reducible;
- give each loop a single exit;
- make the CFG structured: every branch region has a single entry and
  exit, nested properly.

**Why:** Vulkan requires structured control flow (`OpSelectionMerge` and
`OpLoopMerge` for every branch and loop). The SPIR-V backend has its own
structurizer (`SPIRVStructurizer`, `SPIRVMergeRegionExitTargets`), but it
emits invalid control flow for some if/else chains in OSL's code. Giving
it an already structured CFG avoids that.

**Mirrors:** this is AMDGPU's pre-instruction-selection pipeline. AMDGPU
runs LowerSwitch, FixIrreducible, UnifyLoopExits and StructurizeCFG for
the same reason: the GPU's divergent control flow needs a structured CFG.

**Toolchain issue:** *The backend's structurizer produces invalid control
flow*; repro: `cfg_chain.ll`. If the backend's structurizer handled all
reducible CFGs, these passes could go.

## Related workarounds that aren't IR passes

These are listed here for completeness; see TOOLCHAIN_ISSUES.md for each.

- `spirv::fix_structured_merges` (`oslspirv.cpp`): moves instructions that
  the backend places between `OpSelectionMerge` and its branch.
  *Instructions emitted between `OpSelectionMerge` and the branch*;
  `cfg_merge.ll`.
- `-spirvprelegalizercombiner-disable-rule=1`: turns off the crashing
  select-to-FaceForward combine. *Crash in the pre-legalizer combiner*;
  `faceforward.ll`.
- `oslspvemit`: emits in a child process. *Invalid SPIR-V after a few
  modules in the same process*; `same_module_twice.cpp`.
- At the source:
  - closure printing copies the pool into a local (`byref_field_arg.ll`);
  - the matrix shadeops use `fixedIndexInverse` (`select_ptr.ll`).
