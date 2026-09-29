# Project Goal

This branch is a proof of concept to see if OSL materials could run in GPU
graphics pipelines. This is a potentially interesting path for VFX preview
renderers; for studios that have a Vulkan/GL preview renderer (for example)
and a large shader library of OSL nodes (for both bxdfs and pattern generation),
the ability to run final frame shaders (or procedurally simplified final frame
shaders) without needing to rewrite them in GLSL is very appealing.

# Design Breakdown

Since OSL already goes through LLVM, some kind of transformation out of LLVM
IR seems like the obvious solution; since LLVM just added a Spir-V backend,
that's the approach pursued here.  Spir-V has the advantage that even without
access to an AIR or DXIL backend we can add spirv-cross as a pipeline step to
run almost anywhere.

Compiling OSL to Spir-V presents two main sources of excitement: Spir-V requires
structured control flow, and the "Shader" memory model (which we're targetting)
only supports logical pointers. The structured control flow would have been
a bigger worry in the past, but LLVM now has a pass for that (thanks LLVM!).
Most of the excitement of this project has been in rewriting GEPs in a
Spir-V compatible format.

A simpler approach to application integration would perhaps be to implement
OSL evaluation as a deferred shading compute shader. In this case, the OSL
runtime could write the entire shader and tell the applications which input
and output bindings it requires (to satisfy getattribute/texture; to store
closure output).

This branch takes the more complicated approach of attempting to integrate
into a Forward/Forward+ pipeline, where OSL can't be responsible for pipeline
configuration.  Instead, OSL evaluation and resource access are implemented
through a series of callbacks that define an ABI between the OSL shader and the
application shader. At runtime, the OSL shader and the application shader
are compiled separately and linked.  The ABI concept is generically applicable,
but this proof of concept implements the host shader in Slang because of Slang's
support for the "export" keyword (and the fact that the slang compiler is
open source, which lets us fix bugs with the "export" keyword).

Most render services are implemented through this callback interface. Like
the CUDA backend, we explicitly don't support string manipulation in Spir-V,
although we support printf. Most of the RS interface is implemented (see
testsuite coverage), and some of the interface that's stubbed out could be
added to the callback interface easily (e.g. trace, pointcloud).

In addition to the liboslexec changes, this branch adds two new modules:
spirv-tools, and osltoy-vk.

spirv-tools mostly exists as a place to fix up OSL and Slang Spir-V output;
it's separated from liboslexec so as to avoid a spirv-tools dependency. It
can probably go away if the toolchain issues are resolved.

osltoy-vk is a fork of osltoy and shares much of osltoy's code; it was forked
to avoid invasive changes to osltoy during development.  It keeps much of the
osltoy UI, but replaces the grid output with a forward rendered sphere
(reused from testrenderer) that evaluates the input OSL program in the
sphere's fragment shader, and uses the output closure tree to light the
fragment. The runtime defines a simple set of closures (diffuse, microfacet,
emission), and a default OSL input that evalutes a simple PBR material with
a synthetic checkered albedo.  The application also adds a read-only tab to
view the Slang code used in the fragment shader.

A logical next step for osltoy-vk would be to merge it back into osltoy
(adding polymorphism where needed); or to refactor the common shared code
into a library if merging is too difficult or invasive.

## Shader ABI

Slang -> OSL:

[__extern] void eval_osl(inout ShaderGlobals sg, inout Groupdata groupdata);

Shading system state is passed in via the ShaderGlobals and GroupData structs.
The closure tree is built in a member of the groupdata struct.

OSL -> Slang:

interface IOSLRenderer
{
    static uint get_matrix(uint which, uint64_t handle, uint64_t name, float time, uint inverse, inout OSLMatrix result);
    static uint get_attribute(uint64_t object, uint64_t name, uint64_t type, uint derivs, uint index, inout OSLAttribute result);
    static void message(uint kind, uint shade_index, uint64_t format, uint64_t filename, uint nargs, inout OSLMessageArgs args);
    static uint get_userdata(uint64_t name, uint64_t type, uint derivs, inout OSLAttribute result);
    static uint get_params(uint offset, inout OSLAttribute result);
    static uint texture_sample(uint64_t filename, uint swrap, uint twrap, uint interp, uint npoints, inout OSLTextureSample ts);
}

All of the supported render services calls (aside from closures, which write
to groupdata) are implemented via calls to these 6 functions, which are not
defined by OSL but need to be linked in from the application's shader.

# Code changes breakdown

## Build & run steps

Define VULKAN_SDK and VK_DRIVER_FILES in the environment (tested using a
Macbook and KosmicKrisp from SDK 1.4.350.1).

cd ~/OpenShadingLanguage
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/qtbase" \
  -DLLVM_ROOT=/opt/homebrew/opt/llvm@22 \
  -DLLVM_DIRECTORY=/opt/homebrew/opt/llvm@22 \
  -DUSE_LLVM_BITCODE=ON \
  -DOSL_USE_VULKAN=ON \
  -DUSE_QT=ON
ninja -C build

OSL_USE_VULKAN needs an LLVM built with the SPIRV target and USE_LLVM_BITCODE=ON.

Unit tests:
cd build && ctest -j8 -L vulkan
ctest -R unit_spirvpasses           # the lowering-pass unit tests

The 182 vulkan unit tests take ~10m to run.

osltoy-vk:
build/bin/osltoy-vk [-snapshot out.png] [-check] [-script "..."] [path/to/shader.osl]

## OSL includes: src/include/OSL

In Imathx.h, we can't compile the full matrix inverse to Spir-V because of
pointer usage, so we provide an alternate one that's unrolled.

I've added a format string wrapper to fmt_util.h to keep format strings
alive in the host string table, as well as disable ustring construction in
device code (the Cuda backend accomplishes this with an ifdef __CUDA_ARCH__
in the ustring constructor).

I also add TargetISA::SPIRV, spirv_target_machine, spirv_compile_group, etc.

## src/liboslexec

spirv-specific lowering passes are defined in spirv_passes.  Details are written
up in src/spirv-tools/SPIRV_PASSES.md.  We use the builtin LLVM structurizer,
and need to fix pointers to be Spir-V "shader" compatible.  A number of the
passes are workarounds for upstream LLVM issues (as of LLVM 22); see
src/spirv-tools/TOOLCHAIN_ISSUES.md.  Render services are implemented in
llvm_spirv.cpp and rs_spirv.cpp.

There are a few edits to opcolor, opmatrix, optexture, shading_state_uniform,
etc. to address Spir-V limitations.

llvm_spirv.cpp generates a snippet of slang code to define an external interface
to the OSL Spir-V module.  This is generated per-compile so that e.g. we can size
output buffers based on usage, and make sure everything is type-correct, but
we could probably instead steer towards a fixed ABI.

## liboslnoise

gabornoise.h uses a double constant for 2pi in the middle of a float
computation; I've casted this to float so that LLVM doesn't need to. Good
in general for GPUs, where doubles are much more expensive, but especially
on my macbook that doesn't support shader doubles.

## src/spirv-tools

Defines libraries: oslspirv, oslvkcompute; executables: oslspvfix, oslspvemit, oslspvc.

oslspirv is a collection of Spir-V fixup utilities for conditioning LLVM and
Slang output so that we can link it correctly.  oslspvfix, oslspvc, and oslspvemit
are frontends for this library.  oslspvemit runs the OSL->SpirV codegen in
a separate process; this is invoked by BackendLLVM to work around a global state
bug that breaks Spir-V compilation after the first few programs.

oslvkcompute is a vulkan compute shader OSL evaluator, shared by osltoy-vk and
the test harness.

This library can fold into liboslexec, the test code, and osltoy once I address
the toolchain issues, most likely.

## src/osltoy-vk

A mashup of osltoy code, testrenderer code, and some of my own code, designed
to be a shadertoy-like interface that lets you interactively write OSL to run
on a unit sphere.

## src/testshade

Adds a vulkan grid renderer and a "--vulkan" flag to testshade.

## Compiler bug repros

src/spirv-tools/TOOLCHAIN_ISSUES.md
src/spirv-tools/SPIRV_PASSES.md
src/spirv-tools/testdata/...
src/spirv-tools/toolchain_repros/llvm/...
src/spirv-tools/toolchain_repros/slang/...

Issues I've run into with both LLVM22 and Slang, with isolated repro cases.
Upstream fixes would significantly simplify this implementation.
