// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// Unit tests for the SPIR-V lowering passes (spirv_passes.h): each runs on
// a small LLVM IR function, and we check what it rewrote. Where it's the
// workaround for a toolchain bug (see src/spirv-tools/TOOLCHAIN_ISSUES.md),
// the input is the shape that the bug's reproducer uses.

#include <memory>
#include <string>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <OpenImageIO/unittest.h>

#include "spirv_passes.h"

using namespace OSL::pvt;


static const char* header = R"(
target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
)";



struct TestModule {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module;
    llvm::Function* f = nullptr;

    explicit TestModule(const std::string& ir)
    {
        llvm::SMDiagnostic diag;
        module = llvm::parseAssemblyString(header + ir, diag, context);
        if (!module) {
            diag.print("spirv_passes_test", llvm::errs());
            return;
        }
        f = module->getFunction("f");
    }

    template<typename PassT> void run(PassT pass)
    {
        llvm::FunctionPassManager fpm;
        fpm.addPass(std::move(pass));
        run_function_passes(fpm, *f);
    }

    bool valid() const
    {
        return module && f && !llvm::verifyModule(*module, &llvm::errs());
    }

    // How many instructions of `f` satisfy `pred`?
    template<typename Pred> int count(Pred pred) const
    {
        int n = 0;
        for (llvm::BasicBlock& bb : *f)
            for (llvm::Instruction& inst : bb)
                n += pred(inst) ? 1 : 0;
        return n;
    }

    // Is every load and store through a GEP whose source type isn't i8?
    bool all_accesses_typed() const
    {
        bool ok = true;
        for (llvm::BasicBlock& bb : *f)
            for (llvm::Instruction& inst : bb)
                if (llvm::Value* p = llvm::getLoadStorePointerOperand(&inst)) {
                    auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(p);
                    ok &= gep && !gep->getSourceElementType()->isIntegerTy(8);
                }
        return ok;
    }
};



static void
test_fold_null_compares()
{
    // (TOOLCHAIN_ISSUES: comparing a pointer with null; null_compare.ll)
    TestModule m(R"(
%S = type { float }
define spir_func i32 @f() {
  %s = alloca %S
  %c = icmp ne ptr %s, null
  %r = zext i1 %c to i32
  ret i32 %r
}
)");
    m.run(SPIRVFoldNullComparesPass());
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(m.count([](llvm::Instruction& i) {
                         return llvm::isa<llvm::ICmpInst>(i);
                     }),
                     0);
}



static void
test_widen_address_phis()
{
    // (TOOLCHAIN_ISSUES: runtime-indexed loads in a loop;
    // loop_table_index.ll)
    TestModule m(R"(
@t = internal addrspace(10) constant [8 x float] zeroinitializer
define spir_func float @f(i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %idx = zext i32 %i to i64
  %p = getelementptr inbounds [8 x float], ptr addrspace(10) @t, i64 0, i64 %idx
  %v = load float, ptr addrspace(10) %p
  %i1 = add i32 %i, 1
  %c = icmp ult i32 %i1, %n
  br i1 %c, label %loop, label %done
done:
  ret float %v
}
)");
    m.run(SPIRVWidenAddressPhisPass());
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(m.count([](llvm::Instruction& i) {
                         auto* phi = llvm::dyn_cast<llvm::PHINode>(&i);
                         return phi && phi->getType()->isIntegerTy(32);
                     }),
                     0);
    OIIO_CHECK_EQUAL(m.count([](llvm::Instruction& i) {
                         auto* phi = llvm::dyn_cast<llvm::PHINode>(&i);
                         return phi && phi->getType()->isIntegerTy(64);
                     }),
                     1);
}



static void
test_lower_unordered_compares()
{
    // (TOOLCHAIN_ISSUES: unordered/ordered float compares; fcmp_uno.ll)
    TestModule m(R"(
define spir_func i32 @f(float %x, float %y) {
  %a = fcmp uno float %x, %y
  %b = fcmp ord float %x, %y
  %c = and i1 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
)");
    m.run(SPIRVLowerUnorderedComparesPass());
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(m.count([](llvm::Instruction& i) {
                         auto* c = llvm::dyn_cast<llvm::FCmpInst>(&i);
                         return c
                                && (c->getPredicate() == llvm::FCmpInst::FCMP_UNO
                                    || c->getPredicate()
                                           == llvm::FCmpInst::FCMP_ORD);
                     }),
                     0);
}



static void
test_lower_copysign()
{
    // (TOOLCHAIN_ISSUES: llvm.copysign can't be selected; copysign.ll)
    TestModule m(R"(
declare float @llvm.copysign.f32(float, float)
define spir_func float @f(float %x, float %y) {
  %r = call float @llvm.copysign.f32(float %x, float %y)
  ret float %r
}
)");
    m.run(SPIRVLowerCopysignPass());
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(m.count([](llvm::Instruction& i) {
                         auto* c = llvm::dyn_cast<llvm::CallInst>(&i);
                         return c
                                && c->getIntrinsicID()
                                       == llvm::Intrinsic::copysign;
                     }),
                     0);
}



static void
test_localize_globals()
{
    // Mutable internal globals become allocas of the entry; constants
    // don't.
    TestModule m(R"(
@state = internal global [4 x i32] zeroinitializer
@table = internal constant [2 x i32] [i32 1, i32 2]
define spir_func i32 @f(i64 %i, i64 %j) {
entry:
  %p = getelementptr inbounds [4 x i32], ptr @state, i64 0, i64 %i
  store i32 7, ptr %p
  %q = getelementptr inbounds [4 x i32], ptr @state, i64 0, i64 %j
  %v = load i32, ptr %q
  %tp = getelementptr inbounds [2 x i32], ptr @table, i64 0, i64 %j
  %t = load i32, ptr %tp
  %r = add i32 %v, %t
  ret i32 %r
}
)");
    m.run(SPIRVLocalizeGlobalsPass());
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_ASSERT(m.module->getGlobalVariable("state", true) == nullptr);
    OIIO_CHECK_ASSERT(m.module->getGlobalVariable("table", true) != nullptr);
    OIIO_CHECK_EQUAL(m.count([](llvm::Instruction& i) {
                         return llvm::isa<llvm::AllocaInst>(i);
                     }),
                     1);
}



static void
test_localize_globals_constant_index()
{
    // A constant-index access to a localized global (which IRBuilder would
    // fold to a constant expression when rebuilding the GEP).
    TestModule m(R"(
@state = internal global [4 x i32] zeroinitializer
define spir_func i32 @f() {
entry:
  %q = getelementptr inbounds [4 x i32], ptr @state, i64 0, i64 1
  store i32 7, ptr %q
  %v = load i32, ptr %q
  ret i32 %v
}
)");
    m.run(SPIRVLocalizeGlobalsPass());
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_ASSERT(m.module->getGlobalVariable("state", true) == nullptr);
}



static void
test_retype_byte_gep()
{
    // (TOOLCHAIN_ISSUES: untyped pointer arithmetic; untyped_gep.ll)
    TestModule m(R"(
%S = type { float, float, i32 }
define spir_func float @f(ptr byref(%S) %p) {
  %q = getelementptr inbounds i8, ptr %p, i64 4
  %v = load float, ptr %q, align 4
  ret float %v
}
)");
    std::string err;
    m.run(SPIRVRetypePointersPass(&err));
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(err, "");
    OIIO_CHECK_ASSERT(m.all_accesses_typed());
}



static void
test_retype_first_field()
{
    // A load of field 0 through the struct's own pointer gets an explicit
    // all-zero GEP.
    TestModule m(R"(
%S = type { float, float }
define spir_func float @f(ptr byref(%S) %p) {
  %v = load float, ptr %p, align 4
  ret float %v
}
)");
    std::string err;
    m.run(SPIRVRetypePointersPass(&err));
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(err, "");
    OIIO_CHECK_ASSERT(m.all_accesses_typed());
}



static void
test_retype_runtime_index()
{
    // A runtime index into an array of structs, written as the optimizer
    // does: a GEP of the element's size in bytes, then a byte offset.
    TestModule m(R"(
%V = type { float, float, float }
@t = internal addrspace(10) constant [4 x %V] zeroinitializer
define spir_func float @f(i64 %i) {
  %b = getelementptr inbounds [12 x i8], ptr addrspace(10) @t, i64 %i
  %p = getelementptr inbounds i8, ptr addrspace(10) %b, i64 4
  %v = load float, ptr addrspace(10) %p
  ret float %v
}
)");
    std::string err;
    m.run(SPIRVRetypePointersPass(&err));
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(err, "");
    OIIO_CHECK_ASSERT(m.all_accesses_typed());
}



static void
test_retype_same_size_pun()
{
    // Storing the bits of a float field as an int.
    TestModule m(R"(
%S = type { i32, float }
define spir_func void @f(ptr byref(%S) %p, i32 %bits) {
  %q = getelementptr inbounds i8, ptr %p, i64 4
  store i32 %bits, ptr %q, align 4
  ret void
}
)");
    std::string err;
    m.run(SPIRVRetypePointersPass(&err));
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(err, "");
    OIIO_CHECK_ASSERT(m.all_accesses_typed());
}



static void
test_retype_64_across_32()
{
    // A 64-bit load spanning two 32-bit fields.
    TestModule m(R"(
%S = type { i32, i32 }
define spir_func i64 @f(ptr byref(%S) %p) {
  %v = load i64, ptr %p, align 4
  ret i64 %v
}
)");
    std::string err;
    m.run(SPIRVRetypePointersPass(&err));
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(err, "");
    OIIO_CHECK_ASSERT(m.all_accesses_typed());
}



static void
test_retype_memcpy()
{
    // A memcpy between two structs becomes per-field loads and stores.
    TestModule m(R"(
%S = type { float, i32 }
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
define spir_func void @f(ptr byref(%S) %a, ptr byref(%S) %b) {
  call void @llvm.memcpy.p0.p0.i64(ptr %a, ptr %b, i64 8, i1 false)
  ret void
}
)");
    std::string err;
    m.run(SPIRVRetypePointersPass(&err));
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(err, "");
    OIIO_CHECK_EQUAL(m.count([](llvm::Instruction& i) {
                         return llvm::isa<llvm::MemIntrinsic>(i);
                     }),
                     0);
    OIIO_CHECK_ASSERT(m.all_accesses_typed());
}



static void
test_retype_reports_failures()
{
    // A pointer we know nothing about: reported, not guessed.
    TestModule m(R"(
define spir_func float @f(ptr %p) {
  %q = getelementptr inbounds i8, ptr %p, i64 4
  %v = load float, ptr %q, align 4
  ret float %v
}
)");
    std::string err;
    m.run(SPIRVRetypePointersPass(&err));
    OIIO_CHECK_ASSERT(err.find("can't find the type") != std::string::npos);
}



static void
test_fold_handle_casts()
{
    TestModule m(R"(
define spir_func i64 @f(i64 %h) {
  %p = inttoptr i64 %h to ptr
  %r = ptrtoint ptr %p to i64
  ret i64 %r
}
)");
    m.run(SPIRVFoldHandleCastsPass());
    OIIO_CHECK_ASSERT(m.valid());
    OIIO_CHECK_EQUAL(m.count([](llvm::Instruction& i) {
                         return llvm::isa<llvm::IntToPtrInst>(i)
                                || llvm::isa<llvm::PtrToIntInst>(i);
                     }),
                     0);
}



int
main(int argc, char* argv[])
{
    test_fold_null_compares();
    test_widen_address_phis();
    test_lower_unordered_compares();
    test_lower_copysign();
    test_localize_globals();
    test_localize_globals_constant_index();
    test_retype_byte_gep();
    test_retype_first_field();
    test_retype_runtime_index();
    test_retype_same_size_pun();
    test_retype_64_across_32();
    test_retype_memcpy();
    test_retype_reports_failures();
    test_fold_handle_casts();
    return unit_test_failures;
}
