// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// LLVM passes that prepare OSL shader groups for LLVM's SPIR-V backend.
// See spirv_passes.h, and SPIRV_PASSES.md for why each is needed.

#include "spirv_passes.h"

#include <unordered_set>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/ReplaceConstant.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Scalar/StructurizeCFG.h>
#include <llvm/Transforms/Utils/FixIrreducible.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/LowerSwitch.h>
#include <llvm/Transforms/Utils/UnifyLoopExits.h>

#include <OSL/oslconfig.h>


OSL_NAMESPACE_BEGIN
namespace pvt {

namespace {

// Changes of these passes can be anywhere in the function.
inline llvm::PreservedAnalyses
changed(bool c)
{
    return c ? llvm::PreservedAnalyses::none()
             : llvm::PreservedAnalyses::all();
}

}  // namespace



// SPIRVFoldNullComparesPass

llvm::PreservedAnalyses
SPIRVFoldNullComparesPass::run(llvm::Function& F,
                               llvm::FunctionAnalysisManager&)
{
    llvm::Function* func = &F;
    bool modified        = false;
    // Pointers can't be compared with null in logical-addressing SPIR-V
    // (the backend crashes or emits invalid code). Under logical addressing
    // no valid pointer is null, so fold such tests of pointers other than
    // null itself (e.g., "was this optional output given?" after inlining,
    // including of globals cast to the generic address space).
    for (llvm::BasicBlock& bb : *func)
        for (llvm::Instruction& inst : llvm::make_early_inc_range(bb))
            if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst))
                if (cmp->isEquality()
                    && cmp->getOperand(0)->getType()->isPointerTy()
                    && llvm::isa<llvm::ConstantPointerNull>(cmp->getOperand(1))
                    && !llvm::isa<llvm::ConstantPointerNull>(
                        cmp->getOperand(0)->stripPointerCasts())) {
                    cmp->replaceAllUsesWith(llvm::ConstantInt::getBool(
                        cmp->getContext(),
                        cmp->getPredicate() == llvm::ICmpInst::ICMP_NE));
                    cmp->eraseFromParent();
                    modified = true;
                }

    return changed(modified);
}



// SPIRVWidenAddressPhisPass

llvm::PreservedAnalyses
SPIRVWidenAddressPhisPass::run(llvm::Function& F,
                               llvm::FunctionAnalysisManager&)
{
    llvm::Function* func = &F;
    bool modified        = false;
    // LLVM 22's SPIR-V backend crashes ("legalize bitcast") on an array
    // access whose index is computed from a 32-bit phi (e.g., a loop
    // counter), but not from a 64-bit one. So widen 32-bit integer phis
    // that feed address computations, truncating where they're used.
    std::vector<llvm::PHINode*> phis;
    for (llvm::BasicBlock& bb : *func)
        for (llvm::PHINode& phi : bb.phis())
            if (phi.getType()->isIntegerTy(32))
                phis.push_back(&phi);
    auto feeds_address = [](llvm::PHINode* phi) {
        std::vector<llvm::Value*> work { phi };
        std::unordered_set<llvm::Value*> seen;
        while (!work.empty()) {
            llvm::Value* v = work.back();
            work.pop_back();
            if (!seen.insert(v).second)
                continue;
            for (llvm::User* u : v->users()) {
                if (llvm::isa<llvm::GetElementPtrInst>(u))
                    return true;
                if (llvm::isa<llvm::BinaryOperator>(u) || llvm::isa<llvm::CastInst>(u))
                    work.push_back(u);
            }
        }
        return false;
    };
    llvm::Type* i64 = llvm::Type::getInt64Ty(func->getContext());
    for (llvm::PHINode* phi : phis) {
        if (!feeds_address(phi))
            continue;
        llvm::PHINode* wide = llvm::PHINode::Create(i64,
                                                    phi->getNumIncomingValues(),
                                                    phi->getName() + ".wide",
                                                    phi->getIterator());
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            llvm::BasicBlock* pred = phi->getIncomingBlock(i);
            llvm::IRBuilder<> b(pred->getTerminator());
            wide->addIncoming(b.CreateZExt(phi->getIncomingValue(i), i64),
                              pred);
        }
        llvm::IRBuilder<> b(&*phi->getParent()->getFirstInsertionPt());
        phi->replaceAllUsesWith(b.CreateTrunc(wide, phi->getType()));
        phi->eraseFromParent();
        modified = true;
    }

    return changed(modified);
}



// SPIRVLowerUnorderedComparesPass

llvm::PreservedAnalyses
SPIRVLowerUnorderedComparesPass::run(llvm::Function& F,
                                     llvm::FunctionAnalysisManager&)
{
    llvm::Function* func = &F;
    // "Unordered"/"ordered" compares (is either operand NaN?) become
    // OpUnordered/OpOrdered, which Vulkan doesn't allow. Compare each
    // operand with itself instead: x != x (unordered) is true only for NaN.
    std::vector<llvm::FCmpInst*> uno;
    for (llvm::BasicBlock& bb : *func)
        for (llvm::Instruction& inst : bb)
            if (auto* cmp = llvm::dyn_cast<llvm::FCmpInst>(&inst))
                if (cmp->getPredicate() == llvm::FCmpInst::FCMP_UNO
                    || cmp->getPredicate() == llvm::FCmpInst::FCMP_ORD)
                    uno.push_back(cmp);
    for (llvm::FCmpInst* cmp : uno) {
        llvm::IRBuilder<> b(cmp);
        bool unordered = cmp->getPredicate() == llvm::FCmpInst::FCMP_UNO;
        auto isnan     = [&](llvm::Value* v) {
            return b.CreateFCmp(llvm::FCmpInst::FCMP_UNE, v, v);
        };
        llvm::Value* either = b.CreateOr(isnan(cmp->getOperand(0)),
                                         isnan(cmp->getOperand(1)));
        cmp->replaceAllUsesWith(unordered ? either : b.CreateNot(either));
        cmp->eraseFromParent();
    }

    return changed(!uno.empty());
}



// SPIRVLowerCopysignPass

llvm::PreservedAnalyses
SPIRVLowerCopysignPass::run(llvm::Function& F, llvm::FunctionAnalysisManager&)
{
    llvm::Function* func = &F;
    std::vector<llvm::CallInst*> copysigns;
    for (llvm::BasicBlock& bb : *func)
        for (llvm::Instruction& inst : bb)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
                if (call->getIntrinsicID() == llvm::Intrinsic::copysign)
                    copysigns.push_back(call);


    // copysign(mag, sign) with bit operations
    for (llvm::CallInst* call : copysigns) {
        llvm::IRBuilder<> b(call);
        llvm::Type* ftype = call->getType();
        llvm::Type* itype = b.getIntNTy(ftype->getScalarSizeInBits());
        if (auto* vt = llvm::dyn_cast<llvm::VectorType>(ftype))
            itype = llvm::VectorType::get(itype, vt->getElementCount());
        llvm::APInt signbit = llvm::APInt::getSignMask(
            itype->getScalarSizeInBits());
        llvm::Value* mag  = b.CreateAnd(b.CreateBitCast(call->getArgOperand(0),
                                                        itype),
                                        llvm::ConstantInt::get(itype, ~signbit));
        llvm::Value* sign = b.CreateAnd(b.CreateBitCast(call->getArgOperand(1),
                                                        itype),
                                        llvm::ConstantInt::get(itype, signbit));
        call->replaceAllUsesWith(b.CreateBitCast(b.CreateOr(mag, sign), ftype));
        call->eraseFromParent();
    }
    return changed(!copysigns.empty());
}



// SPIRVLocalizeGlobalsPass

namespace {

// Replace pointer `from` (e.g., a global, in any address space) with `to`
// (a generic pointer, e.g., an alloca) in all of its uses, which must be
// instructions in `func`: loads, stores, GEPs, and address space casts.
// Returns false, changing nothing, if it has other uses.
static bool
replace_pointer(llvm::Value* from, llvm::Value* to, llvm::Function* func)
{
    // Check first, so as not to leave a partial rewrite.
    std::vector<llvm::Value*> work { from };
    while (!work.empty()) {
        llvm::Value* v = work.back();
        work.pop_back();
        for (llvm::User* u : v->users()) {
            auto* i = llvm::dyn_cast<llvm::Instruction>(u);
            if (!i || i->getFunction() != func)
                return false;
            if (llvm::isa<llvm::GetElementPtrInst>(i))
                work.push_back(i);
            else if (auto* st = llvm::dyn_cast<llvm::StoreInst>(i)) {
                if (st->getValueOperand() == v)
                    return false;  // the address itself escapes
            } else if (!llvm::isa<llvm::LoadInst>(i)
                       && !llvm::isa<llvm::AddrSpaceCastInst>(i))
                return false;
        }
    }
    // Rewrite, recreating GEPs in the new address space.
    std::function<void(llvm::Value*, llvm::Value*)> rewrite =
        [&](llvm::Value* oldp, llvm::Value* newp) {
            for (llvm::User* u : llvm::make_early_inc_range(oldp->users())) {
                auto* i = llvm::cast<llvm::Instruction>(u);
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(i)) {
                    llvm::IRBuilder<> b(gep);
                    std::vector<llvm::Value*> idx(gep->idx_begin(),
                                                  gep->idx_end());
                    llvm::Value* n = b.CreateGEP(gep->getSourceElementType(),
                                                 newp, idx, gep->getName(),
                                                 gep->getNoWrapFlags());
                    rewrite(gep, n);
                    gep->eraseFromParent();
                } else if (llvm::isa<llvm::AddrSpaceCastInst>(i)
                           && i->getType() == newp->getType()) {
                    i->replaceAllUsesWith(newp);
                    i->eraseFromParent();
                } else if (llvm::isa<llvm::AddrSpaceCastInst>(i)) {
                    llvm::IRBuilder<> b(i);
                    i->replaceAllUsesWith(
                        b.CreateAddrSpaceCast(newp, i->getType()));
                    i->eraseFromParent();
                } else {
                    i->replaceUsesOfWith(oldp, newp);
                }
            }
        };
    rewrite(from, to);
    return true;
}

}  // namespace



llvm::PreservedAnalyses
SPIRVLocalizeGlobalsPass::run(llvm::Function& F,
                              llvm::FunctionAnalysisManager&)
{
    // Mutable globals (the message slots and table) are per-invocation
    // state: make them zero-initialized locals of the entry point, which
    // SROA can then split up. (The optimizer may already have split them
    // into several Private globals; each becomes a local.)
    llvm::Function* entry = &F;
    std::vector<llvm::GlobalVariable*> globals;
    for (llvm::GlobalVariable& g : F.getParent()->globals())
        if (!g.isConstant() && g.hasInitializer() && !g.use_empty()
            && g.hasLocalLinkage())
            globals.push_back(&g);
    // The locals and their initializing stores go at the start of the entry
    // block, in order. (Insert after the last one each time: rewriting the
    // uses may erase the instructions that followed it.)
    llvm::BasicBlock& block = entry->getEntryBlock();
    llvm::Instruction* last = nullptr;
    auto insert_point = [&]() {
        return last ? std::next(last->getIterator())
                    : block.getFirstInsertionPt();
    };
    llvm::IRBuilder<> b(&block, insert_point());
    bool modified = false;
    for (llvm::GlobalVariable* g : globals) {
        llvm::convertUsersOfConstantsToInstructions({ g }, entry);
        b.SetInsertPoint(&block, insert_point());
        llvm::AllocaInst* a = b.CreateAlloca(g->getValueType(), nullptr,
                                             g->getName());
        a->setAlignment(g->getAlign().valueOrOne());
        if (!replace_pointer(g, a, entry)) {
            a->eraseFromParent();
            continue;
        }
        b.SetInsertPoint(a->getParent(), std::next(a->getIterator()));
        last = b.CreateStore(g->getInitializer(), a);
        g->eraseFromParent();
        modified = true;
    }
    return changed(modified);
}



// SPIRVRetypePointersPass

namespace {

// Find the sequence of GEP indices that addresses a value of type `access`
// at byte `offset` within type `type`. Returns false if there isn't one.
static bool
find_field_path(const llvm::DataLayout& layout, llvm::Type* type,
                uint64_t offset, llvm::Type* access,
                std::vector<uint64_t>& path)
{
    if (offset == 0 && type == access)
        return true;
    llvm::Type* elem   = nullptr;
    uint64_t index     = 0;
    uint64_t elem_base = 0;
    if (auto* st = llvm::dyn_cast<llvm::StructType>(type)) {
        const llvm::StructLayout* sl = layout.getStructLayout(st);
        if (st->getNumElements() == 0 || offset >= sl->getSizeInBytes())
            return false;
        index     = sl->getElementContainingOffset(offset);
        elem      = st->getElementType(index);
        elem_base = sl->getElementOffset(index);
    } else if (auto* at = llvm::dyn_cast<llvm::ArrayType>(type)) {
        elem          = at->getElementType();
        uint64_t size = layout.getTypeAllocSize(elem);
        if (!size || offset / size >= at->getNumElements())
            return false;
        index     = offset / size;
        elem_base = index * size;
    } else if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(type)) {
        elem          = vt->getElementType();
        uint64_t size = layout.getTypeAllocSize(elem);
        if (!size || offset / size >= vt->getNumElements())
            return false;
        index     = offset / size;
        elem_base = index * size;
    } else {
        return false;
    }
    path.push_back(index);
    if (find_field_path(layout, elem, offset - elem_base, access, path))
        return true;
    path.pop_back();
    return false;
}



// Collect the scalar fields of `type` that lie entirely within bytes
// [begin, end) (relative to the start of `type`), as (offset, type, path).
struct ScalarField {
    uint64_t offset;
    llvm::Type* type;
    std::vector<uint64_t> path;
};

static bool
scalar_fields_in_range(const llvm::DataLayout& layout, llvm::Type* type,
                       uint64_t base, uint64_t begin, uint64_t end,
                       std::vector<uint64_t>& path,
                       std::vector<ScalarField>& fields)
{
    uint64_t size = layout.getTypeAllocSize(type);
    if (base + size <= begin || base >= end)
        return true;  // disjoint
    if (type->isIntegerTy() || type->isFloatingPointTy()) {
        if (base < begin || base + layout.getTypeStoreSize(type) > end)
            return false;  // straddles the range
        fields.push_back({ base - begin, type, path });
        return true;
    }
    auto recurse = [&](uint64_t index, llvm::Type* elem, uint64_t offset) {
        path.push_back(index);
        bool ok = scalar_fields_in_range(layout, elem, base + offset, begin,
                                         end, path, fields);
        path.pop_back();
        return ok;
    };
    if (auto* st = llvm::dyn_cast<llvm::StructType>(type)) {
        const llvm::StructLayout* sl = layout.getStructLayout(st);
        for (unsigned i = 0; i < st->getNumElements(); ++i)
            if (!recurse(i, st->getElementType(i), sl->getElementOffset(i)))
                return false;
        return true;
    }
    llvm::Type* elem = nullptr;
    uint64_t n       = 0;
    if (auto* at = llvm::dyn_cast<llvm::ArrayType>(type)) {
        elem = at->getElementType();
        n    = at->getNumElements();
    } else if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(type)) {
        elem = vt->getElementType();
        n    = vt->getNumElements();
    } else {
        return false;
    }
    uint64_t esize = layout.getTypeAllocSize(elem);
    for (uint64_t i = 0; i < n; ++i)
        if (!recurse(i, elem, i * esize))
            return false;
    return true;
}



// A GEP instruction, which (unlike IRBuilder's) isn't folded away when
// its indices are all zero: the SPIR-V backend needs the explicit access
// of the first field of a struct.
static llvm::Value*
typed_gep(llvm::IRBuilder<>& builder, llvm::Type* type, llvm::Value* base,
          llvm::ArrayRef<llvm::Value*> indices)
{
    return builder.Insert(
        llvm::GetElementPtrInst::CreateInBounds(type, base, indices));
}



// The state shared by the steps of retyping a function's pointers: how to
// find the type of an object, build typed GEPs into it, and report
// accesses we can't type.
class Retyper {
public:
    Retyper(llvm::Function& F, std::string* err)
        : func(&F)
        , layout(F.getParent()->getDataLayout())
        , i32(llvm::Type::getInt32Ty(F.getContext()))
        , err(err)
    {
    }

    void expand_memops();
    void rewrite_select_addresses();
    void retype_byref_arguments();
    void retype_accesses();
    void delete_dead_addresses();
    bool ok() const { return failures == 0; }
    void finish_report()
    {
        if (err && failures > 10)
            *err += "\n  ... and " + std::to_string(failures - 10) + " more";
    }

private:
    // The type of the object `base` (an alloca, global, or byref
    // parameter), or nullptr if it isn't known.
    llvm::Type* pointee_type(const llvm::Value* base) const
    {
        if (auto* arg = llvm::dyn_cast<llvm::Argument>(base))
            return func->getParamByRefType(arg->getArgNo());
        if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(base))
            return alloca->getAllocatedType();
        if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(base))
            return global->getValueType();
        return nullptr;
    }

    llvm::Type* type_int() const { return i32; }

    llvm::Value* field_gep(llvm::IRBuilder<>& builder, llvm::Type* pointee,
                           llvm::Value* base,
                           const std::vector<uint64_t>& path) const
    {
        std::vector<llvm::Value*> indices { llvm::ConstantInt::get(type_int(),
                                                                   0) };
        for (uint64_t i : path)
            indices.push_back(llvm::ConstantInt::get(type_int(), i));
        return typed_gep(builder, pointee, base, indices);
    }

    void report(llvm::Instruction* inst)
    {
        if (failures++ < 10 && err) {
            std::string s;
            llvm::raw_string_ostream stream(s);
            inst->print(stream);
            if (llvm::Value* ptr = llvm::getLoadStorePointerOperand(inst)) {
                stream << "\n      with address";
                ptr->print(stream);
            }
            *err += "\n  can't find the type of memory accessed by:" + s;
        }
    }

    llvm::Function* func;
    const llvm::DataLayout& layout;
    llvm::Type* i32;
    std::string* err;
    int failures = 0;
};



void
Retyper::expand_memops()
{
    std::vector<uint64_t> path;
    // First expand constant-size memset and memcpy, which the SPIR-V
    // backend can only handle between identically typed objects, into
    // loads and stores of the scalar fields they cover.
    std::vector<llvm::MemIntrinsic*> memops;
    for (llvm::BasicBlock& bb : *func)
        for (llvm::Instruction& inst : bb)
            if (auto* mi = llvm::dyn_cast<llvm::MemIntrinsic>(&inst))
                memops.push_back(mi);
    for (llvm::MemIntrinsic* mi : memops) {
        auto* len = llvm::dyn_cast<llvm::ConstantInt>(mi->getLength());
        auto locate = [&](llvm::Value* ptr, llvm::Value*& base,
                          uint64_t& off) -> llvm::Type* {
            llvm::APInt o(layout.getIndexTypeSizeInBits(ptr->getType()), 0);
            base = ptr->stripAndAccumulateConstantOffsets(layout, o, true);
            off  = o.getZExtValue();
            return pointee_type(base);
        };
        llvm::Value *dbase = nullptr, *sbase = nullptr;
        uint64_t doff = 0, soff = 0;
        llvm::Type* dty = locate(mi->getDest(), dbase, doff);
        std::vector<ScalarField> dfields;
        path.clear();
        if (!len || !dty
            || !scalar_fields_in_range(layout, dty, 0, doff,
                                       doff + len->getZExtValue(), path,
                                       dfields)) {
            report(mi);
            continue;
        }
        llvm::IRBuilder<> builder(mi);
        if (auto* ms = llvm::dyn_cast<llvm::MemSetInst>(mi)) {
            auto* byte = llvm::dyn_cast<llvm::ConstantInt>(ms->getValue());
            if (!byte) {
                report(mi);
                continue;
            }
            for (const ScalarField& fld : dfields) {
                unsigned nbits = layout.getTypeSizeInBits(fld.type);
                llvm::APInt bits = llvm::APInt::getSplat(
                    std::max(8u, nbits), byte->getValue().trunc(8));
                bits = bits.trunc(nbits);
                llvm::Constant* v
                    = fld.type->isIntegerTy()
                          ? (llvm::Constant*)llvm::ConstantInt::get(fld.type,
                                                                    bits)
                          : llvm::ConstantFP::get(
                                fld.type,
                                llvm::APFloat(fld.type->getFltSemantics(),
                                              bits));
                builder.CreateStore(v, field_gep(builder, dty, dbase,
                                                 fld.path));
            }
        } else {
            auto* mt = llvm::cast<llvm::MemTransferInst>(mi);
            llvm::Type* sty = locate(mt->getSource(), sbase, soff);
            std::vector<ScalarField> sfields;
            path.clear();
            bool ok = sty
                      && scalar_fields_in_range(layout, sty, 0, soff,
                                                soff + len->getZExtValue(),
                                                path, sfields)
                      && sfields.size() == dfields.size();
            // Fields must line up, but may have different types of the
            // same size (e.g., copying uint words into floats).
            for (size_t i = 0; ok && i < sfields.size(); ++i)
                ok = sfields[i].offset == dfields[i].offset
                     && layout.getTypeSizeInBits(sfields[i].type)
                            == layout.getTypeSizeInBits(dfields[i].type);
            if (!ok) {
                report(mi);
                continue;
            }
            for (size_t i = 0; i < sfields.size(); ++i) {
                llvm::Value* v = builder.CreateLoad(
                    sfields[i].type,
                    field_gep(builder, sty, sbase, sfields[i].path));
                if (sfields[i].type != dfields[i].type)
                    v = builder.CreateBitCast(v, dfields[i].type);
                builder.CreateStore(v, field_gep(builder, dty, dbase,
                                                 dfields[i].path));
            }
        }
        mi->eraseFromParent();
    }
}



void
Retyper::rewrite_select_addresses()
{
    std::vector<uint64_t> path;
    // Addresses chosen between constant offsets by a select (e.g., which
    // field of a struct to use): SPIR-V can't select pointers, so load both
    // candidates and select the value (or store conditionally to each).
    // Rewrites loads/stores through `gep base, (select c, k1, k2)`.
    {
        std::vector<llvm::Instruction*> sel_accesses;
        for (llvm::BasicBlock& bb : *func)
            for (llvm::Instruction& inst : bb)
                if (llvm::isa<llvm::LoadInst>(inst)
                    || llvm::isa<llvm::StoreInst>(inst))
                    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
                            llvm::getLoadStorePointerOperand(&inst)))
                        if (gep->getNumIndices() == 1
                            && llvm::isa<llvm::SelectInst>(
                                gep->getOperand(1)))
                            sel_accesses.push_back(&inst);
        for (llvm::Instruction* inst : sel_accesses) {
            auto* gep = llvm::cast<llvm::GetElementPtrInst>(
                llvm::getLoadStorePointerOperand(inst));
            auto* sel = llvm::cast<llvm::SelectInst>(gep->getOperand(1));
            if (!llvm::isa<llvm::ConstantInt>(sel->getTrueValue())
                || !llvm::isa<llvm::ConstantInt>(sel->getFalseValue()))
                continue;
            llvm::IRBuilder<> builder(inst);
            auto make = [&](llvm::Value* idx) {
                return builder.CreateGEP(gep->getSourceElementType(),
                                         gep->getPointerOperand(), idx);
            };
            llvm::Value* pt = make(sel->getTrueValue());
            llvm::Value* pf = make(sel->getFalseValue());
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
                llvm::Type* t = load->getType();
                llvm::Value* v = builder.CreateSelect(
                    sel->getCondition(),
                    builder.CreateAlignedLoad(t, pt, load->getAlign()),
                    builder.CreateAlignedLoad(t, pf, load->getAlign()));
                load->replaceAllUsesWith(v);
            } else {
                // store v to (c ? pt : pf) => store (c ? v : *pt) to pt,
                // store (c ? *pf : v) to pf
                auto* store = llvm::cast<llvm::StoreInst>(inst);
                llvm::Value* v = store->getValueOperand();
                llvm::Type* t  = v->getType();
                llvm::Value* ot = builder.CreateAlignedLoad(t, pt,
                                                            store->getAlign());
                llvm::Value* of = builder.CreateAlignedLoad(t, pf,
                                                            store->getAlign());
                builder.CreateAlignedStore(builder.CreateSelect(
                                               sel->getCondition(), v, ot),
                                           pt, store->getAlign());
                builder.CreateAlignedStore(builder.CreateSelect(
                                               sel->getCondition(), of, v),
                                           pf, store->getAlign());
            }
            inst->eraseFromParent();
        }
    }
}



void
Retyper::retype_byref_arguments()
{
    // Pointers passed by reference to declared functions (the renderer's
    // callbacks) must point to the declared type: retype byte-offset
    // pointers to fields of a known object.
    for (llvm::BasicBlock& bb : *func)
        for (llvm::Instruction& inst : bb) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
            if (!call || !call->getCalledFunction())
                continue;
            for (unsigned a = 0; a < call->arg_size(); ++a) {
                llvm::Type* want = call->getParamByRefType(a);
                if (!want)
                    want = call->getCalledFunction()->getParamByRefType(a);
                llvm::Value* ptr = call->getArgOperand(a);
                if (!want || !ptr->getType()->isPointerTy())
                    continue;
                llvm::APInt off(layout.getIndexTypeSizeInBits(ptr->getType()),
                                0);
                llvm::Value* base = ptr->stripAndAccumulateConstantOffsets(
                    layout, off, true);
                llvm::Type* pointee = pointee_type(base);
                std::vector<uint64_t> fpath;
                if (base == ptr || !pointee
                    || !find_field_path(layout, pointee, off.getZExtValue(),
                                        want, fpath))
                    continue;
                std::vector<llvm::Value*> indices {
                    llvm::ConstantInt::get(type_int(), 0)
                };
                for (uint64_t i : fpath)
                    indices.push_back(llvm::ConstantInt::get(type_int(), i));
                llvm::IRBuilder<> builder(call);
                call->setArgOperand(a, typed_gep(builder, pointee, base,
                                                 indices));
            }
        }
}



void
Retyper::retype_accesses()
{
    std::vector<uint64_t> path;
    std::vector<llvm::Instruction*> accesses;
    for (llvm::BasicBlock& bb : *func)
        for (llvm::Instruction& inst : bb)
            if (llvm::isa<llvm::LoadInst>(inst)
                || llvm::isa<llvm::StoreInst>(inst))
                accesses.push_back(&inst);

    for (llvm::Instruction* inst : accesses) {
        llvm::Value* ptr    = llvm::getLoadStorePointerOperand(inst);
        llvm::Type* access  = llvm::getLoadStoreType(inst);
        llvm::APInt offset(layout.getIndexTypeSizeInBits(ptr->getType()), 0);
        llvm::Value* base   = ptr->stripAndAccumulateConstantOffsets(
            layout, offset, /*AllowNonInbounds=*/true);
        llvm::Type* pointee = pointee_type(base);
        path.clear();

        // An address with runtime indices, as the optimizer writes array
        // accesses: a chain of GEPs (of any element types, including
        // addrspacecasts of globals), each adding index * stride bytes.
        // Decompose it into the object, a constant byte offset, and the
        // runtime terms, then index the object's arrays with those.
        if (!pointee) {
            struct Term {
                llvm::Value* index;
                uint64_t stride;
            };
            std::vector<Term> terms;
            uint64_t constant = 0;
            llvm::Value* obj  = ptr;
            bool ok           = true;
            while (ok) {
                llvm::APInt c(offset.getBitWidth(), 0);
                obj = obj->stripAndAccumulateConstantOffsets(layout, c, true);
                constant += c.getZExtValue();
                if (auto* cast = llvm::dyn_cast<llvm::AddrSpaceCastOperator>(
                        obj)) {
                    obj = cast->getPointerOperand();
                    continue;
                }
                auto* gep = llvm::dyn_cast<llvm::GEPOperator>(obj);
                if (!gep)
                    break;
                // Walk the GEP's indices, accumulating strides.
                llvm::Type* t = gep->getSourceElementType();
                for (unsigned i = 1; ok && i < gep->getNumOperands(); ++i) {
                    llvm::Value* idx = gep->getOperand(i);
                    uint64_t stride  = layout.getTypeAllocSize(t);
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(idx))
                        constant += ci->getSExtValue() * stride;
                    else if (i == 1 || !llvm::isa<llvm::StructType>(t))
                        terms.push_back({ idx, stride });
                    else
                        ok = false;
                    if (i + 1 < gep->getNumOperands()) {
                        if (auto* st = llvm::dyn_cast<llvm::StructType>(t)) {
                            auto* ci = llvm::dyn_cast<llvm::ConstantInt>(
                                gep->getOperand(i + 1));
                            if (!ci) {
                                ok = false;
                                break;
                            }
                            constant += layout.getStructLayout(st)
                                            ->getElementOffset(
                                                ci->getZExtValue())
                                        - ci->getZExtValue()
                                              * layout.getTypeAllocSize(
                                                  st->getElementType(
                                                      ci->getZExtValue()));
                            t = st->getElementType(ci->getZExtValue());
                        } else if (auto* at = llvm::dyn_cast<llvm::ArrayType>(
                                       t)) {
                            t = at->getElementType();
                        } else if (auto* vt
                                   = llvm::dyn_cast<llvm::VectorType>(t)) {
                            t = vt->getElementType();
                        } else {
                            ok = false;
                        }
                    }
                }
                obj = gep->getPointerOperand();
            }
            llvm::Type* top = ok ? pointee_type(obj) : nullptr;
            if (top && !terms.empty()) {
                // Descend from the object type by the byte offset, one level
                // at a time. At an array, use a runtime term whose stride
                // is a multiple of the element size.
                llvm::IRBuilder<> builder(inst);
                std::vector<llvm::Value*> indices {
                    llvm::ConstantInt::get(type_int(), 0)
                };
                llvm::Type* t = top;
                uint64_t off  = constant;
                auto as_i32   = [&](llvm::Value* v) {
                    return v->getType()->isIntegerTy(32)
                               ? v
                               : builder.CreateTrunc(v, type_int());
                };
                // The access may be of a different type of the same size as
                // the scalar it reads or writes (e.g., an int of a float).
                auto same_scalar = [&](llvm::Type* x) {
                    return x == access
                           || ((x->isIntegerTy() || x->isFloatingPointTy())
                               && (access->isIntegerTy()
                                   || access->isFloatingPointTy())
                               && layout.getTypeSizeInBits(x)
                                      == layout.getTypeSizeInBits(access));
                };
                while (ok) {
                    if (same_scalar(t) && off == 0 && terms.empty())
                        break;
                    if (auto* st = llvm::dyn_cast<llvm::StructType>(t)) {
                        const llvm::StructLayout* sl = layout.getStructLayout(
                            st);
                        if (off >= sl->getSizeInBytes()) {
                            ok = false;
                            break;
                        }
                        unsigned i = sl->getElementContainingOffset(off);
                        indices.push_back(llvm::ConstantInt::get(type_int(), i));
                        off -= sl->getElementOffset(i);
                        t = st->getElementType(i);
                    } else if (auto* at = llvm::dyn_cast<llvm::ArrayType>(t)) {
                        uint64_t esize = layout.getTypeAllocSize(
                            at->getElementType());
                        // Sum the runtime terms whose strides are multiples
                        // of this element's size. At the innermost array,
                        // terms with smaller strides (e.g., byte offsets
                        // computed as index * size) must also land on an
                        // element: divide their byte total by the size.
                        llvm::Value* index = nullptr;
                        bool innermost = !llvm::isa<llvm::ArrayType>(
                                             at->getElementType())
                                         && !llvm::isa<llvm::StructType>(
                                             at->getElementType());
                        if (innermost) {
                            llvm::Value* bytes = nullptr;
                            for (size_t k = 0; k < terms.size();) {
                                if (terms[k].stride % esize) {
                                    llvm::Value* v = builder.CreateMul(
                                        as_i32(terms[k].index),
                                        llvm::ConstantInt::get(
                                            type_int(), terms[k].stride));
                                    bytes = bytes ? builder.CreateAdd(bytes, v)
                                                  : v;
                                    terms.erase(terms.begin() + k);
                                } else {
                                    ++k;
                                }
                            }
                            if (bytes) {
                                bytes = builder.CreateAdd(
                                    bytes, llvm::ConstantInt::get(type_int(),
                                                                  off % esize));
                                off -= off % esize;
                                index = builder.CreateUDiv(
                                    bytes, llvm::ConstantInt::get(type_int(),
                                                                  esize));
                            }
                        }
                        for (size_t k = 0; k < terms.size();) {
                            if (terms[k].stride % esize == 0) {
                                llvm::Value* v = as_i32(terms[k].index);
                                if (terms[k].stride != esize)
                                    v = builder.CreateMul(
                                        v, llvm::ConstantInt::get(
                                               type_int(),
                                               terms[k].stride / esize));
                                index = index ? builder.CreateAdd(index, v) : v;
                                terms.erase(terms.begin() + k);
                            } else {
                                ++k;
                            }
                        }
                        llvm::Value* cidx = llvm::ConstantInt::get(type_int(),
                                                                   off / esize);
                        indices.push_back(
                            index ? (off / esize ? builder.CreateAdd(index, cidx)
                                                 : index)
                                  : cidx);
                        off %= esize;
                        t = at->getElementType();
                    } else {
                        ok = false;
                    }
                }
                if (ok && terms.empty() && same_scalar(t) && off == 0) {
                    // Access the object's scalar as its declared type, in
                    // its own address space (e.g., a Private global rather
                    // than through a generic cast).
                    llvm::Value* typed = typed_gep(builder, top, obj, indices);
                    auto* load  = llvm::dyn_cast<llvm::LoadInst>(inst);
                    auto* store = llvm::dyn_cast<llvm::StoreInst>(inst);
                    if (typed->getType() == ptr->getType() && t == access) {
                        inst->setOperand(load ? 0 : 1, typed);
                    } else if (load) {
                        llvm::Value* v = builder.CreateAlignedLoad(
                            t, typed, load->getAlign());
                        load->replaceAllUsesWith(
                            builder.CreateBitCast(v, access));
                        load->eraseFromParent();
                    } else {
                        builder.CreateAlignedStore(
                            builder.CreateBitCast(store->getValueOperand(), t),
                            typed, store->getAlign());
                        store->eraseFromParent();
                    }
                    continue;
                }
            }
            path.clear();
        }
        if (pointee
            && find_field_path(layout, pointee, offset.getZExtValue(), access,
                               path)) {
            // Accessing the whole object directly is fine. But an access
            // of its first field through the object's own address (as the
            // optimizer writes field 0 of a struct) needs an explicit GEP,
            // or the SPIR-V backend crashes.
            if (path.empty() && ptr == base)
                continue;
            std::vector<llvm::Value*> indices { llvm::ConstantInt::get(
                type_int(), 0) };
            for (uint64_t i : path)
                indices.push_back(llvm::ConstantInt::get(type_int(), i));
            llvm::IRBuilder<> builder(inst);
            llvm::Value* typed = typed_gep(builder, pointee, base,
                                                           indices);
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(inst))
                load->setOperand(load->getPointerOperandIndex(), typed);
            else {
                auto* store = llvm::cast<llvm::StoreInst>(inst);
                store->setOperand(store->getPointerOperandIndex(), typed);
            }
            continue;
        }
        // The optimizer may access a field as a different type of the same
        // size (e.g., store the integer bits of a float). Access it as its
        // declared type and bitcast the value.
        llvm::Type* field_type = nullptr;
        std::vector<ScalarField> fields;
        path.clear();
        if (pointee
            && (access->isIntegerTy() || access->isFloatingPointTy()
                || access->isPointerTy())) {
            uint64_t begin = offset.getZExtValue();
            if (scalar_fields_in_range(layout, pointee, 0, begin,
                                       begin + layout.getTypeStoreSize(access),
                                       path, fields)
                && fields.size() == 1 && fields[0].offset == 0
                && layout.getTypeSizeInBits(fields[0].type)
                       == layout.getTypeSizeInBits(access)) {
                field_type = fields[0].type;
                path       = fields[0].path;
            }
        }
        // Half of a 64-bit integer field (e.g., a closure handle, which
        // the optimizer copies as 32-bit words): access the field and
        // extract or insert the half (little-endian).
        if (!field_type && pointee
            && (access->isIntegerTy(32) || access->isFloatTy())) {
            uint64_t begin = offset.getZExtValue();
            std::vector<ScalarField> wide;
            std::vector<uint64_t> wpath;
            if (scalar_fields_in_range(layout, pointee, 0, begin & ~7ull,
                                       (begin & ~7ull) + 8, wpath, wide)
                && wide.size() == 1 && wide[0].offset == 0
                && wide[0].type->isIntegerTy(64)) {
                llvm::IRBuilder<> builder(inst);
                llvm::Type* i32 = builder.getInt32Ty();
                llvm::Type* i64 = builder.getInt64Ty();
                unsigned shift  = (begin & 4) ? 32 : 0;
                llvm::Value* p  = field_gep(builder, pointee, base,
                                            wide[0].path);
                llvm::Value* w  = builder.CreateLoad(i64, p);
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
                    llvm::Value* v = builder.CreateTrunc(
                        builder.CreateLShr(w, shift), i32);
                    load->replaceAllUsesWith(builder.CreateBitCast(v, access));
                } else {
                    auto* store = llvm::cast<llvm::StoreInst>(inst);
                    llvm::Value* v = builder.CreateZExt(
                        builder.CreateBitCast(store->getValueOperand(), i32),
                        i64);
                    llvm::Value* keep = builder.CreateAnd(
                        w, llvm::ConstantInt::get(i64, shift ? 0xffffffffull
                                                             : ~0xffffffffull));
                    builder.CreateStore(
                        builder.CreateOr(keep, builder.CreateShl(v, shift)),
                        p);
                }
                inst->eraseFromParent();
                continue;
            }
        }
        // A 64-bit integer (e.g., a string hash) accessed as two 32-bit
        // fields: combine or split the halves (little-endian).
        if (!field_type && access->isIntegerTy(64) && fields.size() == 2
            && fields[0].offset == 0 && fields[1].offset == 4
            && layout.getTypeSizeInBits(fields[0].type) == 32
            && layout.getTypeSizeInBits(fields[1].type) == 32) {
            llvm::IRBuilder<> builder(inst);
            llvm::Type* i32 = builder.getInt32Ty();
            llvm::Value* lo = field_gep(builder, pointee, base, fields[0].path);
            llvm::Value* hi = field_gep(builder, pointee, base, fields[1].path);
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
                auto word = [&](llvm::Value* p, llvm::Type* t) {
                    llvm::Value* v = builder.CreateLoad(t, p);
                    return builder.CreateZExt(builder.CreateBitCast(v, i32),
                                              access);
                };
                llvm::Value* v = builder.CreateOr(
                    word(lo, fields[0].type),
                    builder.CreateShl(word(hi, fields[1].type), 32));
                load->replaceAllUsesWith(v);
            } else {
                auto* store = llvm::cast<llvm::StoreInst>(inst);
                llvm::Value* v = store->getValueOperand();
                builder.CreateStore(
                    builder.CreateBitCast(builder.CreateTrunc(v, i32),
                                          fields[0].type),
                    lo);
                builder.CreateStore(
                    builder.CreateBitCast(
                        builder.CreateTrunc(builder.CreateLShr(v, 32), i32),
                        fields[1].type),
                    hi);
            }
            inst->eraseFromParent();
            continue;
        }
        if (field_type) {
            llvm::IRBuilder<> builder(inst);
            std::vector<llvm::Value*> indices { llvm::ConstantInt::get(
                type_int(), 0) };
            for (uint64_t i : path)
                indices.push_back(llvm::ConstantInt::get(type_int(), i));
            llvm::Value* typed = typed_gep(builder, pointee, base,
                                                           indices);
            // Pointers in SPIR-V structs are 64-bit handles (see
            // llvm_type_sg); convert those rather than bitcasting.
            auto convert = [&](llvm::Value* v, llvm::Type* to) {
                if (to->isPointerTy() && v->getType()->isIntegerTy())
                    return builder.CreateIntToPtr(v, to);
                if (to->isIntegerTy() && v->getType()->isPointerTy())
                    return builder.CreatePtrToInt(v, to);
                return builder.CreateBitCast(v, to);
            };
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
                llvm::Value* v = builder.CreateLoad(field_type, typed);
                load->replaceAllUsesWith(convert(v, access));
            } else {
                auto* store = llvm::cast<llvm::StoreInst>(inst);
                builder.CreateStore(convert(store->getValueOperand(),
                                            field_type),
                                    typed);
            }
            inst->eraseFromParent();
            continue;
        }
        path.clear();
        // The optimizer merges stores of constants to adjacent small fields
        // (e.g., a memset of bool flags) into one wide integer store. Split
        // those back into per-field stores.
        auto* store = llvm::dyn_cast<llvm::StoreInst>(inst);
        auto* cval  = store ? llvm::dyn_cast<llvm::ConstantInt>(
                                 store->getValueOperand())
                            : nullptr;
        fields.clear();
        path.clear();
        uint64_t begin = offset.getZExtValue();
        if (pointee && cval && layout.isLittleEndian()
            && scalar_fields_in_range(layout, pointee, 0, begin,
                                      begin + layout.getTypeStoreSize(access),
                                      path, fields)
            && !fields.empty()) {
            llvm::IRBuilder<> builder(inst);
            const llvm::APInt& bits = cval->getValue();
            for (const ScalarField& fld : fields) {
                unsigned nbits = layout.getTypeSizeInBits(fld.type);
                llvm::APInt fbits = bits.extractBits(nbits, fld.offset * 8);
                llvm::Constant* v = nullptr;
                if (fld.type->isIntegerTy())
                    v = llvm::ConstantInt::get(fld.type,
                                               fld.type->isIntegerTy(1)
                                                   ? fbits.getBoolValue()
                                                   : fbits.getZExtValue());
                else
                    v = llvm::ConstantFP::get(
                        fld.type, llvm::APFloat(fld.type->getFltSemantics(),
                                                fbits));
                std::vector<llvm::Value*> indices { llvm::ConstantInt::get(
                    type_int(), 0) };
                for (uint64_t i : fld.path)
                    indices.push_back(llvm::ConstantInt::get(type_int(), i));
                builder.CreateStore(v, typed_gep(builder, pointee, base,
                                                                 indices));
            }
            inst->eraseFromParent();
            continue;
        }
        // Accesses through typed GEPs we didn't need to touch are fine.
        auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr);
        if (gep && !gep->getSourceElementType()->isIntegerTy(8)
            && gep->getResultElementType() == access)
            continue;
        report(inst);
    }
}



void
Retyper::delete_dead_addresses()
{
    // Remove the now-unused byte-offset GEPs (but don't simplify, which
    // would fold the all-zero GEPs we just made).
    for (llvm::BasicBlock& bb : *func)
        for (llvm::Instruction& inst : llvm::make_early_inc_range(bb))
            llvm::RecursivelyDeleteTriviallyDeadInstructions(&inst);
}

}  // namespace



llvm::PreservedAnalyses
SPIRVRetypePointersPass::run(llvm::Function& F,
                             llvm::FunctionAnalysisManager&)
{
    Retyper r(F, err);
    r.expand_memops();
    r.rewrite_select_addresses();
    r.retype_byref_arguments();
    r.retype_accesses();
    r.delete_dead_addresses();
    r.finish_report();
    if (!r.ok() && err && err->empty())
        *err = "unsupported memory access";
    return llvm::PreservedAnalyses::none();
}



// SPIRVFoldHandleCastsPass

llvm::PreservedAnalyses
SPIRVFoldHandleCastsPass::run(llvm::Function& F,
                              llvm::FunctionAnalysisManager&)
{
    llvm::Function* func = &F;

    std::vector<llvm::Instruction*> casts;
    for (llvm::BasicBlock& bb : *func)
        for (llvm::Instruction& inst : bb)
            if (auto* p2i = llvm::dyn_cast<llvm::PtrToIntInst>(&inst))
                if (auto* i2p = llvm::dyn_cast<llvm::IntToPtrInst>(
                        p2i->getPointerOperand()))
                    if (i2p->getSrcTy() == p2i->getType()) {
                        p2i->replaceAllUsesWith(i2p->getOperand(0));
                        casts.push_back(p2i);
                    }
    for (llvm::Instruction* c : casts)
        c->eraseFromParent();
    for (llvm::BasicBlock& bb : *func)
        for (llvm::Instruction& inst : llvm::make_early_inc_range(bb))
            if (llvm::isa<llvm::IntToPtrInst>(inst) && inst.use_empty())
                inst.eraseFromParent();
    return llvm::PreservedAnalyses::none();
}



// Pipelines

void
add_spirv_lowering_passes(llvm::FunctionPassManager& fpm, std::string* err)
{
    fpm.addPass(SPIRVFoldNullComparesPass());
    fpm.addPass(SPIRVWidenAddressPhisPass());
    fpm.addPass(SPIRVLowerUnorderedComparesPass());
    fpm.addPass(SPIRVLowerCopysignPass());
    fpm.addPass(SPIRVLocalizeGlobalsPass());
    // (Typing must follow the last InstCombine, which makes byte-offset
    // addresses.)
    fpm.addPass(SPIRVRetypePointersPass(err));
    fpm.addPass(SPIRVFoldHandleCastsPass());
}



void
add_spirv_localized_cleanup_passes(llvm::FunctionPassManager& fpm)
{
    fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    fpm.addPass(llvm::InstCombinePass());
    fpm.addPass(llvm::SimplifyCFGPass());
}



void
add_spirv_structurize_passes(llvm::FunctionPassManager& fpm)
{
    fpm.addPass(llvm::LowerSwitchPass());
    fpm.addPass(llvm::FixIrreduciblePass());
    fpm.addPass(llvm::UnifyLoopExitsPass());
    fpm.addPass(llvm::StructurizeCFGPass());
}



void
run_function_passes(llvm::FunctionPassManager& fpm, llvm::Function& F)
{
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    fpm.run(F, fam);
}



void
run_spirv_early_lowering(llvm::Function& entry)
{
    // The lowerings that SROA and InstCombine won't undo.
    {
        llvm::FunctionPassManager fpm;
        fpm.addPass(SPIRVFoldNullComparesPass());
        fpm.addPass(SPIRVWidenAddressPhisPass());
        fpm.addPass(SPIRVLowerUnorderedComparesPass());
        fpm.addPass(SPIRVLowerCopysignPass());
        run_function_passes(fpm, entry);
    }
    // Localize the per-invocation globals, then if there were any, split
    // them into scalars where their accesses allow. (Only then: the cleanup
    // includes InstCombine, which can undo lowerings such as copysign's.)
    {
        llvm::FunctionPassManager fpm;
        fpm.addPass(SPIRVLocalizeGlobalsPass());
        llvm::FunctionAnalysisManager fam;
        llvm::PassBuilder pb;
        pb.registerFunctionAnalyses(fam);
        if (!fpm.run(entry, fam).areAllPreserved()) {
            llvm::FunctionPassManager cleanup;
            add_spirv_localized_cleanup_passes(cleanup);
            run_function_passes(cleanup, entry);
        }
    }
}



void
run_spirv_structurize(llvm::Function& entry)
{
    llvm::FunctionPassManager fpm;
    add_spirv_structurize_passes(fpm);
    run_function_passes(fpm, entry);
}



bool
run_spirv_late_lowering(llvm::Function& entry, std::string& err)
{
    // Typing must follow the last InstCombine, which makes byte-offset
    // addresses.
    err.clear();
    llvm::FunctionPassManager fpm;
    fpm.addPass(SPIRVRetypePointersPass(&err));
    fpm.addPass(SPIRVFoldHandleCastsPass());
    run_function_passes(fpm, entry);
    return err.empty();
}



bool
run_spirv_lowering(llvm::Function& entry, std::string& err)
{
    run_spirv_early_lowering(entry);
    return run_spirv_late_lowering(entry, err);
}

}  // namespace pvt
OSL_NAMESPACE_END
