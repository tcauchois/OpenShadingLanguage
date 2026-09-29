// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// SPIR-V code generation for shader groups: the exported entry point, the
// closure pool and message slots, and the preparation and finalization of
// the module for LLVM's SPIR-V backend. (The Slang ABI is in
// spirv_slang_abi.cpp, and the lowering passes are in spirv_passes.cpp.)

#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/strutil.h>

#include "oslexec_pvt.h"
#include "backendllvm.h"
#include "spirv_abi.h"
#include "spirv_callbacks.h"

#include <llvm/IR/AttributeMask.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/ReplaceConstant.h>

#include "spirv_passes.h"

using namespace OSL::pvt;

OSL_NAMESPACE_BEGIN

namespace pvt {

const char* spirv_entry_name = "eval_osl";

const char* spirv_triple = "spirv1.6-unknown-vulkan1.3-compute";
const char* spirv_data_layout
    = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10";



//
// SPIR-V entry point:
//
//   void eval_osl(ShaderGlobals* sg, Groupdata* groupdata)
//
//  Runs init and the entry layer. The renderer's shader supplies (and
//  may inspect) the groupdata, whose layout is known after compilation.
//  There are no userdata or output buffers, so null pointers are passed
//  for those. Interactive parameters are copied from the renderer (with
//  osl_spirv_get_params) into a local block at entry.
//
llvm::Function*
BackendLLVM::build_llvm_spirv_entry()
{
    ll.current_function(ll.make_function(spirv_entry_name, false,
                                         ll.type_void(),  // return type
                                         { llvm_type_sg_ptr(),
                                           llvm_type_groupdata_ptr() }));

    llvm::BasicBlock* entry_bb = ll.new_basic_block(spirv_entry_name);
    ll.new_builder(entry_bb);

    llvm::Value* null    = ll.void_ptr_null();
    llvm::Value* iparams = null;
    if (size_t size = group().interactive_arena_size()) {
        // Copy the interactive parameters, 16 words at a time, from the
        // renderer's copy of the group's interactive_params block.
        const SpirvCallback& cb = *std::find_if(
            spirv_callbacks().begin(), spirv_callbacks().end(),
            [](const SpirvCallback& c) {
                return !strcmp(c.name, "osl_spirv_get_params");
            });
        llvm::Type* chunk_type = spirv_callback_struct(cb);
        int nwords             = int((size + 3) / 4);
        llvm::Type* block_type = ll.type_array(ll.type_int(), nwords);
        iparams                = ll.op_alloca(block_type, 1, "iparams");
        llvm::Value* chunk     = ll.op_alloca(chunk_type, 1, "iparams_chunk");
        llvm::FunctionType* get_type = llvm::FunctionType::get(
            ll.type_int(), { ll.type_int(), ll.type_ptr(chunk_type) }, false);
        ll.module()->getOrInsertFunction(cb.name, get_type);
        for (int w0 = 0; w0 < nwords; w0 += 16) {
            ll.call_function(cb.name, ll.constant(4 * w0), chunk);
            for (int w = w0; w < std::min(nwords, w0 + 16); ++w) {
                llvm::Value* src = ll.GEP(chunk_type, chunk, 0, 0, w - w0);
                llvm::Value* dst = ll.GEP(block_type, iparams, 0, w);
                ll.op_store(ll.op_load(ll.type_int(), src), dst);
            }
        }
        iparams = ll.void_ptr(iparams);
    }
    llvm::Value* args[] = {
        ll.current_function_arg(0), ll.current_function_arg(1), null, null,
        ll.constant(0),             iparams,
    };

    std::string init_name = init_function_name(shadingsys(), group());
    ll.call_function(init_name.c_str(), args);

    // Run the group's entry layers in order, or if it has none, the last
    // layer. (A layer that has already run, e.g., as a dependency of an
    // earlier one, exits early.)
    int nlayers = group().nlayers();
    if (group().num_entry_layers()) {
        for (int layer : group().m_entry_layer_order)
            ll.call_function(layer_function_name(group(), *group()[layer])
                                 .c_str(),
                             args);
    } else {
        std::string layer_name = layer_function_name(group(),
                                                     *group()[nlayers - 1]);
        ll.call_function(layer_name.c_str(), args);
    }

    ll.op_return();
    ll.end_builder();

    return ll.current_function();
}



// Words in a SPIR-V closure pool node (see rs_spirv.cpp)
static const int spirv_closure_component_words
    = spirv_abi::closure_component_header_words;
static const int spirv_closure_mul_words = spirv_abi::closure_mul_words;
static const int spirv_closure_add_words = spirv_abi::closure_add_words;

int
BackendLLVM::spirv_closure_pool_size()
{
    // The most that the group's closure operations can allocate: each runs
    // at most once per shading point, unless it's in a loop.
    int words     = 0;
    int nodes     = 0;
    bool in_loops = false;
    for (int layer = 0; layer < group().nlayers(); ++layer) {
        ShaderInstance* inst = group()[layer];
        if (inst->unused())
            continue;
        const auto& ops = inst->ops();
        // Ops within loop bodies: [the loop op, its last jump target)
        std::vector<bool> in_loop(ops.size(), false);
        for (size_t i = 0; i < ops.size(); ++i) {
            ustring name = ops[i].opname();
            if (name == "for" || name == "while" || name == "dowhile") {
                int end = ops[i].jump(3);
                for (int j = int(i); j < end && j < int(ops.size()); ++j)
                    in_loop[j] = true;
            }
        }
        for (size_t i = 0; i < ops.size(); ++i) {
            const Opcode& op = ops[i];
            int n            = 0;
            if (op.opname() == "closure") {
                // closure result [weight] name args...
                int namearg = inst->argsymbol(op.firstarg() + 1)
                                      ->typespec()
                                      .is_string()
                                  ? 1
                                  : 2;
                const Symbol* name = inst->argsymbol(op.firstarg() + namearg);
                const ClosureRegistry::ClosureEntry* entry
                    = name->is_constant()
                          ? shadingsys().find_closure(name->get_string())
                          : nullptr;
                n = spirv_closure_component_words
                    + (entry ? (entry->struct_size + 3) / 4 : 0);
            } else if ((op.opname() == "mul" || op.opname() == "add")
                       && inst->argsymbol(op.firstarg())
                              ->typespec()
                              .is_closure_based()) {
                n = op.opname() == "mul" ? spirv_closure_mul_words
                                         : spirv_closure_add_words;
            } else {
                continue;
            }
            words += n;
            ++nodes;
            in_loops |= in_loop[i];
        }
    }
    if (in_loops) {
        words = std::max(words, shadingsys().spirv_closure_pool_words());
        nodes = words / spirv_closure_add_words;  // the smallest node
    }
    m_spirv_closure_max_nodes = nodes;
    return words;
}



llvm::Value*
BackendLLVM::spirv_closure_pool()
{
    OSL_ASSERT(m_spirv_closure_pool_field >= 0);
    return groupdata_field_ref(m_spirv_closure_pool_field);
}



llvm::Type*
BackendLLVM::spirv_closure_pool_type()
{
    if (m_spirv_closure_pool_field < 0)
        return nullptr;
    return llvm::cast<llvm::StructType>(llvm_type_groupdata())
        ->getElementType(m_spirv_closure_pool_field);
}



llvm::Value*
BackendLLVM::llvm_spirv_print_closure(llvm::Value* closure, int opnum,
                                      int element)
{
    // A placeholder that's unique to this printf argument. (The message
    // decoding replaces it with the closure printed just before it by the
    // same shading point, so it needn't be unique across shading points
    // or loop iterations.) Real string hashes never have all these bits.
    uint64_t placeholder = 0xffff000000000000ull
                           | uint64_t(layer() & 0xffff) << 32
                           | uint64_t(opnum & 0xffffff) << 8
                           | uint64_t(element & 0xff);
    llvm::Type* pool_type = spirv_closure_pool_type();
    llvm::FunctionType* ft = llvm::FunctionType::get(
        ll.type_void(),
        { ll.type_int(), ll.type_int64(), ll.type_int(),
          ll.type_ptr(pool_type) },
        false);
    ll.module()->getOrInsertFunction("osl_spirv_print_closure", ft);
    // Pass a copy of the pool: SPIR-V can only pass whole variables by
    // pointer, not a field of the groupdata. (Printing closures is for
    // debugging, so the copy's cost doesn't matter.)
    llvm::Value* copy = ll.op_alloca(pool_type, 1, "closure_pool_copy");
    ll.op_memcpy(copy, spirv_closure_pool(),
                 int(ll.module()->getDataLayout().getTypeAllocSize(pool_type)),
                 4);
    llvm::Value* args[] = {
        ll.call_function("osl_spirv_shade_index", sg_void_ptr()),
        ll.constant64(placeholder),
        ll.op_longlong_to_int(closure),
        copy,
    };
    ll.call_function("osl_spirv_print_closure", args);
    return ll.constant64(placeholder);
}



std::vector<llvm::Value*>
BackendLLVM::spirv_closure_pool_args()
{
    llvm::Type* gd = llvm_type_groupdata();
    int f          = m_spirv_closure_pool_field;
    OSL_ASSERT(f >= 0);
    llvm::Value* words = ll.GEP(gd, groupdata_ptr(), 0, f, 1);
    return { ll.GEP(gd, groupdata_ptr(), 0, f, 0),
             ll.GEP(ll.type_array(ll.type_int(), m_spirv_closure_pool_words),
                    words, 0, 0),
             ll.constant(m_spirv_closure_pool_words) };
}



// The ABI struct `name` (see spirv_abi.h) as the shadeops define it, if
// they use it: clang names it "struct.OSL::<version>::pvt::spirv_abi::name".
static llvm::StructType*
spirv_abi_struct(llvm::Module* module, llvm::StringRef name)
{
    std::string suffix = ("::pvt::spirv_abi::" + name).str();
    for (llvm::StructType* t : module->getIdentifiedStructTypes())
        if (t->hasName() && t->getName().starts_with("struct.OSL::")
            && t->getName().ends_with(suffix))
            return t;
    return nullptr;
}



// A message slot is a header (spirv_abi.h's MessageHeader) and the
// value's 32-bit words, as separate globals.
static llvm::Type*
spirv_message_header(LLVM_Util& ll)
{
    // Prefer the type that rs_spirv.cpp uses, so that the shadeops'
    // accesses match it exactly.
    if (auto* t = spirv_abi_struct(ll.module(), "MessageHeader"))
        return t;
    return ll.type_struct({ ll.type_int64(), ll.type_int64(), ll.type_int(),
                            ll.type_int(), ll.type_int() },
                          "MessageHeader");
}

void
BackendLLVM::assign_spirv_message_slots()
{
    m_spirv_message_slots.clear();
    std::map<ustring, int> words;  // largest value, in words, for each name
    for (int layer = 0; layer < group().nlayers(); ++layer) {
        ShaderInstance* inst = group()[layer];
        if (inst->unused())
            continue;
        for (const Opcode& op : inst->ops()) {
            bool set = op.opname() == "setmessage";
            if (!set && op.opname() != "getmessage")
                continue;
            int namearg = set ? 0 : (op.nargs() == 4 ? 2 : 1);
            const Symbol* name = inst->argsymbol(op.firstarg() + namearg);
            const Symbol* data = inst->argsymbol(op.firstarg() + namearg + 1);
            if (!name->is_constant())
                return;  // Use the table for all messages
            // (Closures are 64-bit handles.)
            TypeDesc t = data->typespec().simpletype();
            int n      = data->typespec().is_closure_based()
                             ? 2 * std::max(1, t.arraylen)
                             : int(t.size() / 4);
            int& w     = words[name->get_string()];
            w          = std::max(w, n);
        }
    }
    auto global = [&](llvm::Type* type, std::string name) {
        return new llvm::GlobalVariable(*ll.module(), type, false,
                                        llvm::GlobalValue::InternalLinkage,
                                        llvm::Constant::getNullValue(type),
                                        name);
    };
    for (auto&& nw : words) {
        SpirvMessageSlot& slot = m_spirv_message_slots[nw.first];
        slot.header = global(spirv_message_header(ll),
                             fmtformat("osl_spirv_message_{}", nw.first));
        slot.data   = global(ll.type_array(ll.type_int(),
                                           std::max(1, nw.second)),
                             fmtformat("osl_spirv_message_{}_data",
                                       nw.first));
    }
}



const BackendLLVM::SpirvMessageSlot*
BackendLLVM::spirv_message_slot(ustring name)
{
    auto found = m_spirv_message_slots.find(name);
    return found == m_spirv_message_slots.end() ? nullptr : &found->second;
}



void
BackendLLVM::prepare_module_for_spirv()
{
    // Vulkan has no function pointers or separately-linked libraries, and
    // the SPIR-V backend handles untyped pointer arguments poorly, so
    // inline everything into the entry point and let SROA turn the
    // resulting allocas into SSA values.
    for (llvm::Function& fn : *ll.module()) {
        if (fn.isDeclaration() || fn.getName() == spirv_entry_name)
            continue;
        fn.removeFnAttr(llvm::Attribute::NoInline);
        fn.addFnAttr(llvm::Attribute::AlwaysInline);
        // Fastcall isn't meaningful for SPIR-V.
        fn.setCallingConv(llvm::CallingConv::SPIR_FUNC);
    }
    for (llvm::Function& fn : *ll.module()) {
        fn.setCallingConv(llvm::CallingConv::SPIR_FUNC);
        // The shadeops were compiled for the host; drop its CPU tuning.
        fn.removeFnAttr("target-cpu");
        fn.removeFnAttr("target-features");
        fn.removeFnAttr("tune-cpu");
        for (llvm::BasicBlock& bb : fn)
            for (llvm::Instruction& inst : bb)
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
                    call->setCallingConv(llvm::CallingConv::SPIR_FUNC);
    }

    // C++ static initialization isn't used, and SPIR-V can't represent it.
    if (llvm::GlobalVariable* ctors
        = ll.module()->getGlobalVariable("llvm.global_ctors"))
        ctors->eraseFromParent();
}



llvm::Type*
BackendLLVM::spirv_callback_struct(const SpirvCallback& cb)
{
    // Prefer the type that rs_spirv.cpp's callers allocate, so that the
    // arguments' types match the declaration exactly.
    if (auto* t = spirv_abi_struct(ll.module(),
                                   std::string("Spirv")
                                       + (cb.struct_name + 3)))  // not "OSL"
        return t;
    if (auto* t = llvm::StructType::getTypeByName(ll.context(), cb.struct_name))
        return t;
    // Parse the struct's fields, all arrays of 32-bit words.
    std::vector<llvm::Type*> fields;
    for (auto&& f : Strutil::splitsv(cb.fields, ";")) {
        auto words = Strutil::splitsv(Strutil::strip(f), " []");
        if (words.size() < 3)
            continue;
        llvm::Type* elem = words[0] == "float" ? ll.type_float()
                                               : ll.type_int();
        fields.push_back(ll.type_array(elem, Strutil::stoi(words[2])));
    }
    return ll.type_struct(fields, cb.struct_name);
}



// Build a constant of `type` whose memory image is `bytes` (host layout,
// which must match the module's data layout).
static llvm::Constant*
constant_from_bytes_impl(const llvm::DataLayout& layout, llvm::Type* type,
                         const uint8_t* bytes)
{
    if (auto* st = llvm::dyn_cast<llvm::StructType>(type)) {
        const llvm::StructLayout* sl = layout.getStructLayout(st);
        std::vector<llvm::Constant*> fields;
        for (unsigned i = 0; i < st->getNumElements(); ++i)
            fields.push_back(constant_from_bytes_impl(
                layout, st->getElementType(i), bytes + sl->getElementOffset(i)));
        return llvm::ConstantStruct::get(st, fields);
    }
    if (auto* at = llvm::dyn_cast<llvm::ArrayType>(type)) {
        uint64_t esize = layout.getTypeAllocSize(at->getElementType());
        std::vector<llvm::Constant*> elems;
        for (uint64_t i = 0; i < at->getNumElements(); ++i)
            elems.push_back(constant_from_bytes_impl(
                layout, at->getElementType(), bytes + i * esize));
        return llvm::ConstantArray::get(at, elems);
    }
    if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(type)) {
        uint64_t esize = layout.getTypeAllocSize(vt->getElementType());
        std::vector<llvm::Constant*> elems;
        for (unsigned i = 0; i < vt->getNumElements(); ++i)
            elems.push_back(constant_from_bytes_impl(
                layout, vt->getElementType(), bytes + i * esize));
        return llvm::ConstantVector::get(elems);
    }
    if (type->isIntegerTy()) {
        uint64_t v = 0;
        memcpy(&v, bytes, layout.getTypeStoreSize(type));
        return llvm::ConstantInt::get(type, v);
    }
    if (type->isFloatTy()) {
        float f;
        memcpy(&f, bytes, sizeof(f));
        return llvm::ConstantFP::get(type, f);
    }
    if (type->isDoubleTy()) {
        double d;
        memcpy(&d, bytes, sizeof(d));
        return llvm::ConstantFP::get(type, d);
    }
    if (type->isPointerTy())  // Opaque handles aren't meaningful on the GPU
        return llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(type));
    OSL_ASSERT(0 && "unhandled type in constant_from_bytes");
    return nullptr;
}



llvm::Constant*
BackendLLVM::constant_from_bytes(llvm::Type* type, const uint8_t* bytes)
{
    return constant_from_bytes_impl(ll.module()->getDataLayout(), type, bytes);
}



void
BackendLLVM::finalize_spirv(llvm::Function* entry)
{
    llvm::Type* arg_pointees[] = { llvm_type_sg(), llvm_type_groupdata() };
    // Inferred parameter attributes (readonly, etc.) become Kernel-only
    // decorations in SPIR-V, and aren't part of our ABI anyway. Mark the
    // pointee types explicitly with byref so that the SPIR-V parameter
    // types are right even if the shader never uses a parameter.
    for (unsigned i = 0; i < entry->arg_size(); ++i) {
        entry->removeParamAttrs(i, llvm::AttributeMask(
                                       entry->getAttributes().getParamAttrs(
                                           i)));
        entry->addParamAttr(i, llvm::Attribute::getWithByRefType(
                                   ll.context(), arg_pointees[i]));
    }
    // Lowerings the SPIR-V backend needs (see spirv_passes.h), part 1
    run_spirv_early_lowering(*entry);

    // Give the shadeops the shading system's uniform state (color system,
    // etc.) as a constant, since they can't follow the ShaderGlobals'
    // pointer to it. The struct is plain data, and SPIR-V's layout of
    // scalars and arrays matches the host's, so copy its bytes.
    if (llvm::GlobalVariable* ssu = ll.module()->getGlobalVariable(
            "osl_spirv_shading_state_uniform")) {
        const ShadingStateUniform& host = shadingsys().shading_state_uniform();
        llvm::Type* type                = ssu->getValueType();
        if (ll.module()->getDataLayout().getTypeAllocSize(type)
            != sizeof(ShadingStateUniform)) {
            shadingcontext()->errorfmt(
                "SPIR-V ShadingStateUniform layout doesn't match the host's");
            return;
        }
        ssu->setInitializer(constant_from_bytes(
            type, reinterpret_cast<const uint8_t*>(&host)));
        ssu->setConstant(true);
        ssu->setLinkage(llvm::GlobalValue::InternalLinkage);
    }

    if (llvm::GlobalVariable* strict = ll.module()->getGlobalVariable(
            "osl_spirv_strict_messages")) {
        strict->setInitializer(
            ll.constant(int(shadingsys().strict_messages())));
        strict->setConstant(true);
        strict->setLinkage(llvm::GlobalValue::InternalLinkage);
    }

    // Functions the renderer's shader supplies (see rs_spirv.cpp). Their
    // pointer parameters point to structs of 32-bit words; declare those
    // types so the SPIR-V signatures match the Slang declarations.
    for (const SpirvCallback& cb : spirv_callbacks()) {
        llvm::Function* fn = ll.module()->getFunction(cb.name);
        if (!fn)
            continue;
        fn->setCallingConv(llvm::CallingConv::SPIR_FUNC);
        unsigned last = fn->arg_size() - 1;
        fn->removeParamAttrs(last, llvm::AttributeMask(
                                       fn->getAttributes().getParamAttrs(last)));
        fn->addParamAttr(last, llvm::Attribute::getWithByRefType(
                                   ll.context(), spirv_callback_struct(cb)));
    }

    // The renderer's closure printer takes the closure pool.
    if (llvm::Function* fn = ll.module()->getFunction(
            "osl_spirv_print_closure")) {
        fn->setCallingConv(llvm::CallingConv::SPIR_FUNC);
        fn->addParamAttr(3, llvm::Attribute::getWithByRefType(
                                ll.context(), spirv_closure_pool_type()));
    }

    // Anything else still called but not defined would become an import
    // that the renderer can't supply.
    std::vector<std::string> missing;
    for (llvm::Function& fn : *ll.module())
        if (fn.isDeclaration() && !fn.isIntrinsic() && !fn.use_empty()
            && !is_spirv_callback(fn.getName())
            && fn.getName() != "osl_spirv_print_closure")
            missing.push_back(fn.getName().str());
    if (missing.size()) {
        shadingcontext()->errorfmt(
            "Shader group \"{}\" uses functions not yet supported for SPIR-V: {}",
            group().name(), Strutil::join(missing, ", "));
        return;
    }

    // Lowerings, part 2: the optimizer's byte-offset addresses need their
    // types back for logical addressing.
    std::string err;
    bool typed_ok = run_spirv_late_lowering(*entry, err);
    if (!typed_ok) {
        shadingcontext()->errorfmt(
            "Unsupported memory access for SPIR-V in shader group \"{}\":{}",
            group().name(), err);
        return;
    }

    // Vulkan requires structured control flow. The SPIR-V backend's own
    // structurizer produces invalid code for some of the CFGs the optimizer
    // creates (e.g., if/else chains whose branches exit to a common block),
    // so normalize them first.
    run_spirv_structurize(*entry);

    if (llvm_debug() >= 2 || shadingsys().llvm_output_bitcode()) {
        // The complete module as given to the SPIR-V backend, which can be
        // fed to llc to reproduce backend problems.
        std::string name = fmtformat(
            "{}_spirv.ll",
            Strutil::replace(Strutil::replace(group().name(), "/", "_", true),
                             ":", "_", true));
        if (OIIO::Filesystem::write_text_file(name, ll.module_string()))
            shadingsys().infofmt("Wrote SPIR-V module IR to '{}'", name);
    }

    if (!ll.spirv_compile_group(group().m_llvm_spirv_compiled_version)) {
        group().m_llvm_spirv_compiled_version.clear();
        shadingcontext()->errorfmt(
            "Unable to generate SPIR-V for shader group \"{}\" (the LLVM "
            "SPIR-V backend failed; set llvm_output_bitcode to save the "
            "module, {}_spirv.ll, to reproduce it with llc)",
            group().name(), group().name());
        return;
    }
    group().m_spirv_slang_abi = spirv_slang_abi();
}


llvm::Value*
BackendLLVM::spirv_add_closures(llvm::Value* a, llvm::Value* b)
{
    std::vector<llvm::Value*> args = spirv_closure_pool_args();
    args.insert(args.begin(), sg_void_ptr());
    args.push_back(a);
    args.push_back(b);
    return ll.call_function("osl_spirv_add_closure_closure", args);
}



llvm::Value*
BackendLLVM::spirv_mul_closure(llvm::Value* closure, llvm::Value* weight,
                               bool tfloat)
{
    std::vector<llvm::Value*> args = spirv_closure_pool_args();
    args.insert(args.begin(), sg_void_ptr());
    args.push_back(closure);
    args.push_back(weight);
    return ll.call_function(tfloat ? "osl_spirv_mul_closure_float"
                                   : "osl_spirv_mul_closure_color",
                            args);
}



llvm::Value*
BackendLLVM::spirv_allocate_closure(const ClosureRegistry::ClosureEntry& entry,
                                    const Opcode& op, llvm::Value* weight)
{
    if (entry.prepare || entry.setup) {
        shadingcontext()->errorfmt(
            "Closure '{}' has prepare or setup functions, which aren't "
            "supported for SPIR-V ({}:{})",
            entry.name, op.sourcefile(), op.sourceline());
        return nullptr;
    }
    std::vector<llvm::Value*> args = spirv_closure_pool_args();
    args.insert(args.begin(), sg_void_ptr());
    args.push_back(ll.constant(entry.id));
    args.push_back(ll.constant((entry.struct_size + 3) / 4));
    args.push_back(weight ? weight : ll.void_ptr_null());
    return ll.call_function("osl_spirv_allocate_closure_component", args);
}



void
BackendLLVM::spirv_closure_param(llvm::Value* handle, int offset,
                                 llvm::Value* src, int size)
{
    OSL_DASSERT(offset % 4 == 0 && size % 4 == 0);
    llvm::Value* args[] = { spirv_closure_pool_args()[1], handle,
                            ll.constant(offset / 4), src,
                            ll.constant(size / 4) };
    ll.call_function("osl_spirv_closure_param", args);
}



llvm::Value*
BackendLLVM::spirv_getmessage(const Symbol& name, llvm::Value* const* args)
{
    const SpirvMessageSlot* slot = name.is_constant()
                                       ? spirv_message_slot(name.get_string())
                                       : nullptr;
    if (!slot)
        return nullptr;
    llvm::Value* slot_args[11] = { args[0], slot->header, slot->data,
                                   args[1], args[2],      args[3],
                                   args[4], args[5],      args[6],
                                   args[7], args[8] };
    return ll.call_function("osl_spirv_getmessage_slot", slot_args);
}



bool
BackendLLVM::spirv_setmessage(const Symbol& name, llvm::Value* const* args)
{
    const SpirvMessageSlot* slot = name.is_constant()
                                       ? spirv_message_slot(name.get_string())
                                       : nullptr;
    if (!slot)
        return false;
    llvm::Value* slot_args[9] = { args[0], slot->header, slot->data,
                                  args[1], args[2],      args[3],
                                  args[4], args[5],      args[6] };
    ll.call_function("osl_spirv_setmessage_slot", slot_args);
    return true;
}

}  // namespace pvt

OSL_NAMESPACE_END
