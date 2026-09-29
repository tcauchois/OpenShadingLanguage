# Known toolchain issues affecting the SPIR-V backend

Bugs and limitations found in LLVM's SPIR-V backend and in Slang while
developing OSL's SPIR-V (Vulkan) support, with the workarounds in place.
Tested with LLVM 22.1.8 and Slang 2026.8 (Vulkan SDK 1.4.350.1).

Standalone reproducers are in `toolchain_repros/` (referred to as
*Repro:* below). Each directory has a `run.sh` that runs them all:

    cd toolchain_repros/llvm
    LLC=/path/to/llc SPIRV_VAL=/path/to/spirv-val ./run.sh *.ll
    cd ../slang
    SLANGC=/path/to/slangc ./run.sh *.slang

The LLVM reproducers are compute entry points (or library functions, as
OSL's shader groups are) compiled with `llc -O2 -filetype=obj` for
`spirv1.6-unknown-vulkan1.3-compute`. The Slang ones use
`-target spirv -incomplete-library`.

When a workaround gets too involved, we note the issue here and move on:
affected tests are marked `NOVULKAN` in the testsuite.

Most of the LLVM workarounds are IR passes run before emission, in
`src/liboslexec/spirv_passes.{h,cpp}`; [SPIRV_PASSES.md](SPIRV_PASSES.md)
describes each one and the issue it works around.

## LLVM SPIR-V backend

- **Untyped pointer arithmetic crashes the backend** ("SPIRV legalize
  bitcast pass"). The optimizer canonicalizes struct and array accesses to
  byte offsets (`getelementptr i8, ...`), which the backend can't type
  under logical addressing. *Repro:* `untyped_gep.ll` (a byte GEP to a
  field of a `byref` struct; `typed_gep_ok.ll` is the typed equivalent,
  which compiles). *Workaround:* `SPIRVRetypePointersPass` rewrites
  accesses back into typed GEPs, including splitting merged stores and
  memset/memcpy into per-field accesses. It reports anything it can't type.

- **Loading the first field of a struct through the struct's own pointer**
  (no GEP) crashed in OSL shader groups. We couldn't reproduce it in
  isolation (small cases compile), so it may depend on the context.
  *Workaround:* the typing pass emits explicit all-zero GEPs, and avoids
  simplifications that would fold them.

- **Runtime-indexed loads from a table in a loop crash the backend**
  ("SPIRV legalize bitcast pass"), with 32- or 64-bit loop counters, even
  with fully typed GEPs. They compile with `-disable-lsr`, so the address
  rewrite by loop strength reduction seems to be the trigger. *Repro:*
  `loop_table_index.ll` (an array of floats), `loop_table_vec3.ll` (an
  array of 3-float structs); `loop_table_bytegep.ll` is the same with the
  byte-offset address the optimizer writes, which crashes even with
  `-disable-lsr`. *Workaround:* partial -- 32-bit phis feeding addresses are
  widened to 64 bits. **Open:** `spline`, `spline-boundarybug`,
  `splineinverse-ident`, `debug-uninit` (which uses `spline`), `initlist`,
  `pointcloud-fold`, and `blackbody` are `NOVULKAN`. (`-disable-lsr` alone
  doesn't fix the spline shadeops.)

- **Selecting or phi-ing between pointers, then loading through the
  result, crashes the backend** ("SPIRV legalize bitcast pass") under
  logical addressing (pointer phis need the VariablePointers capability).
  This is what Imath's `Matrix44::inverse` does when it swaps pivot rows.
  *Repro:* `select_ptr.ll`. *Workaround:* the typing pass rewrites selects
  of addresses into selects of the loaded values, and the SPIR-V shadeops
  use `fixedIndexInverse` (Imathx.h), the same algorithm with the pivot
  swap done by value, which gives identical results.

- **Comparing a pointer with null** (`icmp ne ptr %p, null`, including
  `addrspacecast`s of globals) crashes the backend ("SPIRV legalize bitcast
  pass"). *Repro:* `null_compare.ll`. *Workaround:* `SPIRVFoldNullComparesPass`
  folds such comparisons (no valid pointer is null under logical addressing).

- **Small mutable globals are split into Private globals with `i1`
  fields** by the optimizer (e.g., a message slot's state); in OSL shader
  groups those crashed the backend. A Private `i1` global alone compiles,
  so this too depends on the context. *Workaround:* `SPIRVLocalizeGlobalsPass`
  turns the shadeops' mutable globals (message state) into
  zero-initialized locals of the entry point, and SROA splits them into
  scalars.

- **A pointer to a field of a struct can't be passed to a function** taking
  a `byref` argument: the backend emits an `OpFunctionCall` whose pointer
  operand is an access chain, which `spirv-val` rejects ("Pointer operand
  must be a memory object declaration"). *Repro:* `byref_field_arg.ll`.
  *Workaround:* pass a copy in a local variable (closure printing does this
  for the closure pool).

- **Unordered/ordered float compares** (`fcmp uno`/`ord`, i.e. "is either
  operand NaN?") become `OpUnordered`/`OpOrdered`, which require the Kernel
  capability, so the module fails Vulkan validation. *Repro:*
  `fcmp_uno.ll`. *Workaround:* rewritten as `x != x` tests.

- **The backend's structurizer produces invalid control flow** for some
  if/else chains ("exits the selection ... but not via a structured exit").
  *Repro:* `cfg_chain.ll` (an if/else-if chain with an early exit to the
  common successor). *Workaround:* run LLVM's `lower-switch`,
  `fix-irreducible`, `unify-loop-exits`, and `structurizecfg` before
  emission.

- **Instructions emitted between `OpSelectionMerge` and the branch,**
  which `spirv-val` rejects. Here, the backend combines two identical
  selects (one in a branch, one after it) into one, which it places in the
  header block after the merge. *Repro:* `cfg_merge.ll`. *Workaround:*
  `spirv::fix_structured_merges` moves them before the merge at link time.

- **Crash in the pre-legalizer combiner's select-to-FaceForward rule**
  (rule 1: a null dereference in "SPIRVPreLegalizerCombiner"), on float
  selects with constant operands. *Repro:* `faceforward.ll` (reduced from
  OSL's gabor noise; compiles with
  `-spirvprelegalizercombiner-disable-rule=1`). *Workaround:* disable rule
  1.

- **`llvm.copysign` can't be selected** ("cannot select: G_FCOPYSIGN").
  *Repro:* `copysign.ll`. *Workaround:* lowered to bit operations after
  optimization (the optimizer recreates it otherwise).

- **Invalid SPIR-V after a few modules in the same process** ("Id N is
  defined more than once"): the backend keeps state between modules, even
  with a fresh `LLVMContext` and `TargetMachine` for each, and on the same
  thread or not. Compiling the same (valid) module repeatedly in one process
  gives a good result the first few times, then bad ones from then on
  (e.g., `....XX`); separate `llc` processes always succeed. Found as
  intermittent "Shader group build failed" errors in osltoy-vk after a few
  recompiles. *Repro:* `same_module_twice.cpp` compiles a module N times
  in one process; build it against LLVM (see its comment) and run it on
  `same_module_input.ll` (an OSL shader group's IR), then validate each
  output. *Workaround:* the shading system emits each
  group's SPIR-V in a separate process, with the `oslspvemit` tool (found
  next to the library, next to the program, or in the PATH; or set
  `OSL_SPIRV_EMITTER`, or `none` to emit in-process). It takes about 25 ms.

- **No address space maps to `PhysicalStorageBuffer`,** so buffer device
  addresses can't be used directly (see the design discussion).

- The backend's crashes would take down the process; `spirv_compile_group`
  runs it under `llvm::CrashRecoveryContext` and reports a compile error.

## Slang

- **Crash (segfault in `slangc`) compiling a shader that calls an
  `[__extern]` function when a function kept for linking contains a loop
  that isn't unrolled.** *Repro:* `loop_crash.slang` (compiles with
  `[ForceUnroll]` on the loop, or without the call to the `[__extern]`
  function). *Workaround:* the renderer callbacks use fixed-bound
  `[ForceUnroll]` loops.

- **Crash (segfault in `slangc`) on a `switch` statement in a shader that
  calls an `[__extern]` function,** in some shapes: here, a `switch` in a
  `[mutating]` struct method called from an unrolled loop. The same code
  compiles with an `if` in place of the `switch`, or without the call to the
  `[__extern]` function. Found in osltoy-vk's closure lighting code (a
  `switch` on the closure id in the `IOSLClosureVisitor`), which is where
  renderers would naturally write one. *Repro:* `switch_crash.slang` (40
  lines). The crash is sensitive to the surrounding code: the obvious
  smaller versions (a free function, a single call, no loop) compile.
  *Workaround:* if/else chains instead of `switch` in renderer shader code.

- **Slow compiles of nested unrolled loops over a struct passed by
  value:** a tree walker over a word pool, calling a visitor through a
  generic interface that copies each node's words, takes about 35 seconds
  and produces 1.9 MB of SPIR-V. (A real loop instead crashes; see the first
  item.) *Repro:* `slow_unroll.slang`. *Workaround:* the walkers are
  bounded by the group's node count, and closure printing copies the pool
  to the host and walks it there.

- **`[__extern]` declarations are emitted as bodiless functions without an
  Import linkage decoration** (or the Linkage capability), and `export`
  functions aren't decorated either, so the module can't be linked.
  *Repro:* `extern_stub.slang` (see its comment for what to look for in the
  disassembly). *Workaround:* `spirv::make_imports` / `make_exports`.

- **Unused exported functions are removed,** even with `export`, and a
  module with only exports is rejected ("no exported symbols").
  *Repro:* `export_removed.slang`. *Workaround:* the generated
  `osl_keep_callbacks()`, which the kernel calls under a condition that's
  never true at run time. The calls must have visible effects, or they're
  removed too.

- **Types must match exactly across the link:** Slang uses signed `int` for
  array lengths and literals, LLVM unsigned. *Workaround:* the generated ABI
  uses unsigned types throughout, and the linker allows pointer-parameter
  type mismatches.

## Drivers

- KosmicKrisp and MoltenVK (Apple GPUs) don't support `shaderFloat64`, so
  shadeops must not use `double` (see the `gabornoise.h` fix).
- KosmicKrisp doesn't support timestamp queries (`timestampValidBits = 0`)
  or `VK_KHR_pipeline_executable_properties`, so osltoy-vk can't report GPU
  times or the final shader statistics there.
