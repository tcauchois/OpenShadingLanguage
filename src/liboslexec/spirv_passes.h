// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

// LLVM passes that prepare an OSL shader group's entry function (with
// everything inlined into it) for LLVM's SPIR-V backend, for Vulkan. Most
// work around limitations or bugs in the backend; see SPIRV_PASSES.md for
// why each is needed, and TOOLCHAIN_ISSUES.md (in src/spirv-tools) for the
// bugs.
//
// They run after the optimizer, which would otherwise undo them, in the
// order of add_spirv_lowering_passes(). The entry function's pointer
// parameters must have byref attributes giving their pointee types.

#include <string>

#include <llvm/IR/PassManager.h>

#include <OSL/oslconfig.h>


OSL_NAMESPACE_BEGIN
namespace pvt {

/// Fold `icmp eq/ne ptr %p, null` to a constant: under logical addressing
/// no valid pointer is null.
struct SPIRVFoldNullComparesPass
    : public llvm::PassInfoMixin<SPIRVFoldNullComparesPass> {
    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& AM);
};

/// Widen 32-bit integer phis that feed address computations to 64 bits.
struct SPIRVWidenAddressPhisPass
    : public llvm::PassInfoMixin<SPIRVWidenAddressPhisPass> {
    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& AM);
};

/// Rewrite `fcmp uno`/`fcmp ord` (which need the Kernel capability) as
/// `x != x` tests.
struct SPIRVLowerUnorderedComparesPass
    : public llvm::PassInfoMixin<SPIRVLowerUnorderedComparesPass> {
    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& AM);
};

/// Lower `llvm.copysign` to bit operations.
struct SPIRVLowerCopysignPass
    : public llvm::PassInfoMixin<SPIRVLowerCopysignPass> {
    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& AM);
};

/// Turn the module's mutable internal globals (per-invocation state, such
/// as the shadeops' message slots and table) into zero-initialized
/// allocas of this (entry) function, so that SROA can split them up.
/// Follow it with SROA (see add_spirv_lowering_passes).
struct SPIRVLocalizeGlobalsPass
    : public llvm::PassInfoMixin<SPIRVLocalizeGlobalsPass> {
    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& AM);
};

/// Rewrite memory accesses through byte-offset (`getelementptr i8`) and
/// other untyped addresses into typed GEPs from their objects (allocas,
/// globals, and byref parameters), which the SPIR-V backend needs for
/// logical addressing. Accesses it can't type are described in `*err`
/// (if given), and leave the module unusable for SPIR-V.
struct SPIRVRetypePointersPass
    : public llvm::PassInfoMixin<SPIRVRetypePointersPass> {
    explicit SPIRVRetypePointersPass(std::string* err = nullptr) : err(err) {}
    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& AM);
    std::string* err;
};

/// Fold `ptrtoint(inttoptr(x))` to x (from 64-bit handles in the
/// ShaderGlobals, which the shadeops convert to and from pointers).
struct SPIRVFoldHandleCastsPass
    : public llvm::PassInfoMixin<SPIRVFoldHandleCastsPass> {
    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& AM);
};

/// Add the lowering passes, in order. SPIRVRetypePointersPass reports to
/// `*err`. (This omits the cleanup after SPIRVLocalizeGlobalsPass, which
/// run_spirv_lowering runs only if it localized something.)
void
add_spirv_lowering_passes(llvm::FunctionPassManager& fpm, std::string* err);

/// The cleanup after SPIRVLocalizeGlobalsPass: SROA, InstCombine, and
/// SimplifyCFG.
void
add_spirv_localized_cleanup_passes(llvm::FunctionPassManager& fpm);

/// Add the passes that give the function structured control flow, as
/// Vulkan requires (and that the SPIR-V backend's own structurizer doesn't
/// always produce): LowerSwitch, FixIrreducible, UnifyLoopExits, and
/// StructurizeCFG.
void
add_spirv_structurize_passes(llvm::FunctionPassManager& fpm);

/// Run a function pass manager on `F` with a complete set of analysis
/// managers (some passes need module and loop analyses).
void
run_function_passes(llvm::FunctionPassManager& fpm, llvm::Function& F);

/// Run the lowering passes on `entry`. Returns false and sets `err` if
/// some accesses couldn't be typed.
bool
run_spirv_lowering(llvm::Function& entry, std::string& err);

/// run_spirv_lowering in two parts: the lowerings and localization of
/// globals (with its cleanup), then the pointer typing and handle casts.
/// The shading system sets up the module's constants in between.
void
run_spirv_early_lowering(llvm::Function& entry);
bool
run_spirv_late_lowering(llvm::Function& entry, std::string& err);

/// Give `entry` structured control flow (add_spirv_structurize_passes).
/// This follows the lowerings.
void
run_spirv_structurize(llvm::Function& entry);

}  // namespace pvt
OSL_NAMESPACE_END
