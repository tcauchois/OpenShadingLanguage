// Compile one LLVM IR module to SPIR-V N times in one process (default 6),
// each time with a fresh LLVMContext and TargetMachine, writing
// /tmp/same_module_N.spv. After a few compiles, the outputs fail
// spirv-val ("Id N is defined more than once") though the first ones pass.
//
//   clang++ -std=c++17 same_module_twice.cpp -o same_module_twice \
//       $(llvm-config --cxxflags --ldflags --libs all --system-libs)
//   ./same_module_twice module.ll 6
//   for f in /tmp/same_module_*.spv; do spirv-val --target-env spv1.6 $f; done
// Compile the same LLVM IR module to SPIR-V twice in one process, with a
// fresh LLVMContext and TargetMachine each time, and write both outputs.
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Support/FileSystem.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    int runs = argc > 2 ? atoi(argv[2]) : 6;
    for (int run = 0; run < runs; ++run) {
        llvm::LLVMContext ctx;
        llvm::SMDiagnostic err;
        auto mod = llvm::parseIRFile(argv[1], err, ctx);
        if (!mod) { err.print("twice", llvm::errs()); return 1; }
        std::string e;
        const llvm::Target* t = llvm::TargetRegistry::lookupTarget(mod->getTargetTriple(), e);
        llvm::TargetOptions opts;
        llvm::TargetMachine* tm = t->createTargetMachine(mod->getTargetTriple(), "", "", opts, llvm::Reloc::Static);
        llvm::legacy::PassManager pm;
        std::error_code ec;
        std::string out = "/tmp/same_module_" + std::to_string(run) + ".spv";
        llvm::raw_fd_ostream os(out, ec, llvm::sys::fs::OF_None);
        if (tm->addPassesToEmitFile(pm, os, nullptr, llvm::CodeGenFileType::ObjectFile)) return 2;
        pm.run(*mod);
        os.flush();
        delete tm;
        printf("wrote %s\n", out.c_str());
    }
}
