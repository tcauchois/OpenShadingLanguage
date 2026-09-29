// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// oslspvemit: run LLVM's SPIR-V backend on one module, in its own process.
//
//   oslspvemit input.bc output.spv
//
// LLVM 22's SPIR-V backend keeps state between modules compiled in the same
// process, so after a few modules it starts emitting invalid SPIR-V ("Id N
// is defined more than once"), even with a fresh LLVMContext and
// TargetMachine. The shading system works around that by having this tool
// emit each shader group's SPIR-V (see LLVM_Util::spirv_compile_group).
// The input is the prepared module, ready for emission; the tool only
// emits it, with the same options the shading system would use.

#include <cstdio>
#include <string>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>


int
main(int argc, char* argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: oslspvemit input.bc output.spv\n");
        return 2;
    }
    LLVMInitializeSPIRVTargetInfo();
    LLVMInitializeSPIRVTarget();
    LLVMInitializeSPIRVTargetMC();
    LLVMInitializeSPIRVAsmPrinter();

    // Work around a crash in the pre-legalizer combiner's
    // select-to-FaceForward rule (see TOOLCHAIN_ISSUES.md).
    const char* opts[] = { "oslspvemit",
                           "-spirvprelegalizercombiner-disable-rule=1" };
    llvm::cl::ParseCommandLineOptions(2, opts);

    llvm::LLVMContext context;
    auto buffer = llvm::MemoryBuffer::getFile(argv[1]);
    if (!buffer) {
        fprintf(stderr, "oslspvemit: can't read %s\n", argv[1]);
        return 1;
    }
    auto module = llvm::parseBitcodeFile((*buffer)->getMemBufferRef(),
                                         context);
    if (!module) {
        fprintf(stderr, "oslspvemit: can't parse %s: %s\n", argv[1],
                llvm::toString(module.takeError()).c_str());
        return 1;
    }

    llvm::Triple triple((*module)->getTargetTriple());
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple,
                                                                    err);
    if (!target) {
        fprintf(stderr, "oslspvemit: %s\n", err.c_str());
        return 1;
    }
    // (As LLVM_Util::spirv_target_machine.)
    llvm::TargetOptions options;
    options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        triple, "", "", options, llvm::Reloc::Static, llvm::CodeModel::Small,
        llvm::CodeGenOptLevel::Default));

    std::error_code ec;
    llvm::raw_fd_ostream out(argv[2], ec, llvm::sys::fs::OF_None);
    if (ec) {
        fprintf(stderr, "oslspvemit: can't write %s\n", argv[2]);
        return 1;
    }
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, out, nullptr,
                                llvm::CodeGenFileType::ObjectFile)) {
        fprintf(stderr, "oslspvemit: can't emit SPIR-V\n");
        return 1;
    }
    pm.run(**module);
    out.flush();
    return 0;
}
