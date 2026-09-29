// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// Generation of the Slang side of the SPIR-V ABI for a shader group: the
// declarations that a renderer's shader includes to call the group (see
// BackendLLVM::spirv_slang_abi).

#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>

#include <OpenImageIO/strutil.h>

#include "oslexec_pvt.h"
#include "backendllvm.h"
#include "spirv_abi.h"
#include "spirv_callbacks.h"

#include <llvm/IR/AttributeMask.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

using namespace OSL::pvt;

OSL_NAMESPACE_BEGIN

namespace pvt {



const std::vector<SpirvCallback>&
spirv_callbacks()
{
    static const std::vector<SpirvCallback> callbacks = {
        { "osl_spirv_get_matrix", "OSLMatrix", "float m[" OSL_SPIRV_STRINGIZE(OSL_SPIRV_MATRIX_FLOATS) "];", "uint",
          "get_matrix",
          "uint which, uint64_t handle, uint64_t name, float time, "
          "uint inverse, inout OSLMatrix result",
          "which, handle, name, time, inverse, result",
          "osl_spirv_get_matrix(0, 0, 0, 0, 0, s) + uint(s.m[0])" },
        { "osl_spirv_get_attribute", "OSLAttribute", "uint words[" OSL_SPIRV_STRINGIZE(OSL_SPIRV_ATTRIBUTE_WORDS) "];",
          "uint", "get_attribute",
          "uint64_t object, uint64_t name, uint64_t type, uint derivs, "
          "uint index, inout OSLAttribute result",
          "object, name, type, derivs, index, result",
          "osl_spirv_get_attribute(0, 0, 0, 0, 0, s) + s.words[0]" },
        { "osl_spirv_message", "OSLMessageArgs",
          "uint types[" OSL_SPIRV_STRINGIZE(OSL_SPIRV_MAX_MESSAGE_ARGS) "]; uint words[" OSL_SPIRV_STRINGIZE(OSL_SPIRV_MAX_MESSAGE_WORDS) "];", "void", "message",
          "uint kind, uint shade_index, uint64_t format, uint64_t filename, "
          "uint nargs, inout OSLMessageArgs args",
          "kind, shade_index, format, filename, nargs, args",
          "s.words[0]" },
        { "osl_spirv_get_userdata", "OSLAttribute", "uint words[" OSL_SPIRV_STRINGIZE(OSL_SPIRV_ATTRIBUTE_WORDS) "];",
          "uint", "get_userdata",
          "uint64_t name, uint64_t type, uint derivs, inout OSLAttribute result",
          "name, type, derivs, result",
          "osl_spirv_get_userdata(0, 0, 0, s) + s.words[0]" },
        { "osl_spirv_get_params", "OSLAttribute", "uint words[" OSL_SPIRV_STRINGIZE(OSL_SPIRV_ATTRIBUTE_WORDS) "];", "uint",
          "get_params", "uint offset, inout OSLAttribute result",
          "offset, result", "osl_spirv_get_params(0, s) + s.words[0]" },
        { "osl_spirv_texture_sample", "OSLTextureSample",
          "float st[" OSL_SPIRV_STRINGIZE(OSL_SPIRV_TEXTURE_ST_FLOATS)
          "]; float dx[2]; float dy[2]; float rgba["
          OSL_SPIRV_STRINGIZE(OSL_SPIRV_TEXTURE_RGBA_FLOATS)
          "]; uint info[4];",
          "uint", "texture_sample",
          "uint64_t filename, uint swrap, uint twrap, uint interp, "
          "uint npoints, inout OSLTextureSample ts",
          "filename, swrap, twrap, interp, npoints, ts",
          "osl_spirv_texture_sample(0, 0, 0, 0, 0, s) + uint(s.rgba[0])" },
    };
    return callbacks;
}



bool
is_spirv_callback(llvm::StringRef name)
{
    for (const SpirvCallback& cb : spirv_callbacks())
        if (name == cb.name)
            return true;
    return false;
}

// Slang spelling of an LLVM type used in the SPIR-V ABI. Named structs
// are declared (dependencies first) into `decls`.
static std::string
slang_type_name(llvm::Type* type, std::string& decls,
                std::unordered_set<llvm::Type*>& declared,
                std::vector<std::string>* field_names = nullptr,
                std::string* array_suffix = nullptr);

static std::string
slang_array_suffix(llvm::Type*& type)
{
    std::string suffix;
    while (auto* at = llvm::dyn_cast<llvm::ArrayType>(type)) {
        suffix += fmtformat("[{}]", at->getNumElements());
        type = at->getElementType();
    }
    return suffix;
}

static std::string
slang_type_name(llvm::Type* type, std::string& decls,
                std::unordered_set<llvm::Type*>& declared,
                std::vector<std::string>* field_names,
                std::string* array_suffix)
{
    if (array_suffix)
        *array_suffix = slang_array_suffix(type);
    if (type->isFloatTy())
        return "float";
    if (type->isIntegerTy(1))
        return "bool";
    if (type->isIntegerTy(8))
        return "uint8_t";
    if (type->isIntegerTy(16))
        return "uint16_t";
    // SPIR-V integers from LLVM are unsigned, so the Slang types must be
    // too or the linker reports a type mismatch.
    if (type->isIntegerTy(32))
        return "uint";
    if (type->isIntegerTy(64))
        return "uint64_t";
    // Pointers (e.g., closures in groupdata) are opaque to the renderer.
    if (type->isPointerTy())
        return "uint64_t";
    auto* st = llvm::dyn_cast<llvm::StructType>(type);
    OSL_ASSERT(st && st->hasName() && "unhandled type in SPIR-V ABI");
    std::string name = st->getName().str();
    if (declared.insert(st).second) {
        std::string body;
        for (unsigned i = 0; i < st->getNumElements(); ++i) {
            std::string suffix;
            std::string ft = slang_type_name(st->getElementType(i), decls,
                                             declared, nullptr, &suffix);
            std::string fname = field_names && i < field_names->size()
                                    ? (*field_names)[i]
                                    : fmtformat("f{}", i);
            body += fmtformat("    {} {}{};\n", ft, fname, suffix);
        }
        decls += fmtformat("struct {} {{\n{}}};\n\n", name, body);
    }
    return name;
}



std::string
BackendLLVM::spirv_slang_abi()
{
    std::string decls;
    std::unordered_set<llvm::Type*> declared;

    std::vector<std::string> xyz = { "x", "y", "z" };
    slang_type_name(ll.type_triple(), decls, declared, &xyz);

    // Name the ShaderGlobals fields as in OSL (P, dPdz, ...). The
    // derivative-carrying fields are arrays of [value, dx, dy].
    auto* sg = llvm::cast<llvm::StructType>(llvm_type_sg());
    std::vector<std::string> sg_names;
    for (unsigned i = 0; i < sg->getNumElements(); ++i)
        sg_names.emplace_back(ShaderGlobalIndexToName(i).string());
    slang_type_name(sg, decls, declared, &sg_names);

    // Name the Groupdata fields after the params they hold. Params are
    // prefixed by their layer name (just the param name for the last
    // layer, which is where renderer outputs usually live).
    auto* gd = llvm::cast<llvm::StructType>(llvm_type_groupdata());
    std::vector<std::string> gd_names = m_groupdata_field_names;
    // Renderer outputs may be set per group or for the whole system.
    const std::vector<ustring>& outputs
        = group().m_renderer_outputs.size()
              ? group().m_renderer_outputs
              : shadingsys().renderer_outputs();
    auto& out_fields  = group().m_spirv_output_fields;
    std::vector<int> out_layer(outputs.size(), -1);
    out_fields.assign(outputs.size(), std::string());
    for (auto&& p : m_param_order_map) {
        const Symbol* sym = p.first;
        const ShaderInstance* inst = group()[sym->layer()];
        std::string name = Strutil::replace(sym->name(), ".", "_", true);
        if (sym->layer() != group().nlayers() - 1)
            name = fmtformat("{}_{}", inst->layername(), name);
        gd_names[p.second] = name;
        // Match renderer outputs named "param" or "layer.param", preferring
        // later layers as the CPU path does.
        if (!sym->renderer_output())
            continue;
        for (size_t i = 0; i < outputs.size(); ++i) {
            if ((outputs[i] == sym->name()
                 || outputs[i]
                        == fmtformat("{}.{}", inst->layername(), sym->name()))
                && sym->layer() > out_layer[i]) {
                out_layer[i]  = sym->layer();
                out_fields[i] = name;
            }
        }
    }
    if (llvm::Type* pool = spirv_closure_pool_type()) {
        std::vector<std::string> pool_names = { "used", "words" };
        slang_type_name(pool, decls, declared, &pool_names);
    }
    slang_type_name(gd, decls, declared, &gd_names);

    std::string abi = "// Generated by OSL for shader group \"" + group().name().string()
                      + "\" -- do not edit.\n\n" + decls;
    abi += fmtformat("[__extern] void {}(inout ShaderGlobals sg, "
                     "inout Groupdata groupdata);\n\n",
                     spirv_entry_name);
    abi += spirv_slang_closures();

    // Codes passed to the callbacks (see spirv_abi.h).
    using namespace spirv_abi;
    abi += fmtformat(
        "// Kinds of message (osl_spirv_message's kind)\n"
        "static const uint OSL_MESSAGE_ERROR = {};\n"
        "static const uint OSL_MESSAGE_WARNING = {};\n"
        "static const uint OSL_MESSAGE_PRINT = {};\n"
        "static const uint OSL_MESSAGE_FILEPRINT = {};\n\n"
        "// Which matrix osl_spirv_get_matrix is asked for: by transformation "
        "handle or by name\n"
        "static const uint OSL_MATRIX_FROM_HANDLE = {};\n"
        "static const uint OSL_MATRIX_FROM_NAME = {};\n\n",
        uint32_t(Error), uint32_t(Warning), uint32_t(Print),
        uint32_t(FilePrint),
        uint32_t(MatrixFromHandle), uint32_t(MatrixFromName));

    // Renderer callbacks. The renderer's shader implements the
    // IOSLRenderer interface, and instantiates OSL_RENDERER_CALLBACKS(R) at
    // global scope with its implementation R. That defines the callbacks
    // for linking, and osl_keep_callbacks(), which the kernel must call
    // (e.g., under a condition that's never true at run time) so that Slang
    // doesn't remove them before they're linked with the shader group.
    abi += "// Renderer callbacks\n";
    std::string iface, defs, keep;
    std::set<std::string> structs;
    for (const SpirvCallback& cb : spirv_callbacks()) {
        if (structs.insert(cb.struct_name).second)
            abi += fmtformat("struct {} {{ {} }};\n", cb.struct_name,
                             cb.fields);
        iface += fmtformat("    static {} {}({});\n", cb.ret, cb.method,
                           cb.params);
        // Forward the callback to the renderer's implementation.
        bool is_void = !strcmp(cb.ret, "void");
        defs += fmtformat(
            "    [noinline] export {} {}({}) {{ {}R::{}({}); }} \\\n",
            cb.ret, cb.name, cb.params, is_void ? "" : "return ", cb.method,
            cb.args);
        // Calls must have visible effects to survive.
        keep += fmtformat("        {{ {} s = {{}}; {}{}; }} \\\n",
                          cb.struct_name,
                          is_void ? fmtformat("{}(0, 0, 0, 0, 0, s); r += ",
                                              cb.name)
                                  : "r += ",
                          cb.keep_call);
    }
    abi += fmtformat("\ninterface IOSLRenderer\n{{\n{}}}\n\n", iface);
    abi += fmtformat("#define OSL_RENDERER_CALLBACKS(R) \\\n{}"
                     "    uint osl_keep_callbacks() \\\n"
                     "    {{ \\\n        uint r = 0; \\\n{}"
                     "        return r; \\\n    }}\n",
                     defs, keep);
    return abi;
}



// Slang spelling of a closure param's type, and whether it's supported.
static std::string
slang_closure_param_type(TypeDesc t, std::string& suffix)
{
    suffix.clear();
    if (t.arraylen > 0)
        suffix = fmtformat("[{}]", t.arraylen);
    TypeDesc e = t.elementtype();
    if (e == TypeDesc::FLOAT)
        return "float";
    if (e == TypeDesc::INT)
        return "int";
    if (e.basetype == TypeDesc::FLOAT && e.aggregate == TypeDesc::VEC3)
        return "float3";
    if (e == TypeDesc::STRING)
        return "uint64_t";  // string hash
    if (e == TypeDesc::PTR)
        return "uint";  // closure handle
    return "";
}



// Slang code to read a closure param of type t from pool word `at` (an
// expression), assigning it to `dst`.
static std::string
slang_closure_param_read(TypeDesc t, const std::string& dst,
                         const std::string& at)
{
    TypeDesc e  = t.elementtype();
    int n       = std::max(1, t.arraylen);
    int ewords  = int(e.size() / 4);
    std::string code;
    for (int i = 0; i < n; ++i) {
        std::string d = t.arraylen > 0 ? fmtformat("{}[{}]", dst, i) : dst;
        std::string w = fmtformat("{} + {}", at, i * ewords);
        if (e == TypeDesc::FLOAT)
            code += fmtformat("    {} = asfloat(pool.words[{}]);\n", d, w);
        else if (e == TypeDesc::INT)
            code += fmtformat("    {} = int(pool.words[{}]);\n", d, w);
        else if (e.aggregate == TypeDesc::VEC3)
            code += fmtformat(
                "    {0} = float3(asfloat(pool.words[{1}]), "
                "asfloat(pool.words[{1} + 1]), "
                "asfloat(pool.words[{1} + 2]));\n",
                d, w);
        else if (e == TypeDesc::STRING)
            code += fmtformat(
                "    {0} = uint64_t(pool.words[{1}]) "
                "| (uint64_t(pool.words[{1} + 1]) << 32);\n",
                d, w);
        else if (e == TypeDesc::PTR)
            code += fmtformat("    {} = pool.words[{}];\n", d, w);
    }
    return code;
}



// The closure part of the Slang ABI: an id for each closure the renderer
// registered (so that renderer code can be written once, for any shader),
// and if the group has closures, a struct and unpack function for each
// closure, and a walker that calls a visitor for each component.
std::string
BackendLLVM::spirv_slang_closures()
{
    std::string ids, unpack;
    for (const ClosureRegistry::ClosureEntry& c :
         shadingsys().closure_registry().entries()) {
        // (A name may be registered more than once; OSL uses the last.)
        if (c.name.empty() || shadingsys().find_closure(c.name) != &c)
            continue;
        ids += fmtformat("static const uint OSL_CLOSURE_{} = {};\n", c.name,
                         c.id);
        if (m_spirv_closure_pool_field < 0)
            continue;
        // A struct with the closure's params, named p0, p1, ... in the
        // order of the closure's arguments, or by keyword.
        std::string fields, reads;
        bool ok = true;
        for (int i = 0; i < int(c.params.size()) - 1; ++i) {
            const ClosureParam& p = c.params[i];
            std::string suffix;
            std::string type = slang_closure_param_type(p.type, suffix);
            if (type.empty() || p.offset % 4) {
                ok = false;
                break;
            }
            std::string name = p.key ? std::string(p.key) : fmtformat("p{}", i);
            fields += fmtformat("    {} {}{};\n", type, name, suffix);
            reads += slang_closure_param_read(p.type, "c." + name,
                                              fmtformat("node + {}",
                                                        spirv_abi::closure_component_header_words + p.offset / 4));
        }
        if (!ok)
            continue;  // Params we can't describe: no struct
        unpack += fmtformat("struct OSLClosure_{0} {{\n{1}}};\n"
                            "OSLClosure_{0} osl_closure_{0}("
                            "OSLClosurePool pool, uint node)\n{{\n"
                            "    OSLClosure_{0} c;\n{2}"
                            "    return c;\n}}\n\n",
                            c.name, fields.size() ? fields : "    uint unused;\n",
                            reads);
    }

    std::string s = "// Closures. Their values (e.g., sg.Ci) are handles: 1 + "
                    "the index of their\n"
                    "// node in the groupdata's closure_pool, or 0 for no "
                    "closure. Nodes are\n"
                    "//   component: id, weight (3 floats), params\n"
                    "//   mul: OSL_CLOSURE_MUL, weight (3 floats), handle\n"
                    "//   add: OSL_CLOSURE_ADD, handle, handle\n";
    s += fmtformat("static const uint OSL_CLOSURE_MUL = {:#x};\n"
                   "static const uint OSL_CLOSURE_ADD = {:#x};\n",
                   spirv_abi::ClosureMulId, spirv_abi::ClosureAddId);
    s += ids + "\n";
    if (m_spirv_closure_pool_field < 0)
        return s;
    s += unpack;
    // printf of a closure calls a function the renderer defines:
    s += "// printf of a closure calls osl_spirv_print_closure, which the "
         "renderer must define\n"
         "// (and keep, like the callbacks) if the group prints closures:\n"
         "//   [noinline] export void osl_spirv_print_closure(uint "
         "shade_index,\n"
         "//       uint64_t placeholder, uint root, inout OSLClosurePool "
         "pool)\n"
         "// It formats the closure (or not), and has the printf's message "
         "decoding\n"
         "// substitute it for the placeholder string hash.\n\n";
    s += fmtformat("// The most nodes the group's closure pool can hold\n"
                   "static const int OSL_CLOSURE_MAX_NODES = {};\n\n",
                   std::max(1, m_spirv_closure_max_nodes));
    s += R"(// Calls v.component(id, weight, node, pool) for each component of the
// closure with handle `root` (e.g., uint(sg.Ci)) in `pool` (the
// groupdata's closure_pool), with its weight including those of the mul
// nodes above it. (A component's params start at word node + 4; read them
// with osl_closure_<name>(pool, node).)
interface IOSLClosureVisitor
{
    [mutating] void component(uint id, float3 w, uint node,
                              OSLClosurePool pool);
}

static const int OSL_CLOSURE_STACK = OSL_CLOSURE_MAX_NODES;

void osl_walk_closures<V : IOSLClosureVisitor>(OSLClosurePool pool,
                                               uint root, inout V v)
{
    uint stack[OSL_CLOSURE_STACK];
    float3 weights[OSL_CLOSURE_STACK];
    int depth = 0;
    uint handle = root;
    float3 w = float3(1, 1, 1);
    // (Visiting each node at most once. Unrolled: Slang 2026.8 can crash
    // on loops in shaders that call [__extern] functions.)
    [ForceUnroll] for (int iter = 0; iter < OSL_CLOSURE_MAX_NODES; ++iter) {
        if (handle == 0) {
            if (depth == 0)
                break;
            --depth;
            handle = stack[depth];
            w = weights[depth];
        }
        uint node = handle - 1;
        uint id = pool.words[node];
        if (id == OSL_CLOSURE_MUL) {
            w *= float3(asfloat(pool.words[node + 1]),
                        asfloat(pool.words[node + 2]),
                        asfloat(pool.words[node + 3]));
            handle = pool.words[node + 4];
        } else if (id == OSL_CLOSURE_ADD) {
            if (depth < OSL_CLOSURE_STACK) {
                stack[depth] = pool.words[node + 2];
                weights[depth] = w;
                ++depth;
            }
            handle = pool.words[node + 1];
        } else {
            v.component(id, w * float3(asfloat(pool.words[node + 1]),
                                       asfloat(pool.words[node + 2]),
                                       asfloat(pool.words[node + 3])),
                        node, pool);
            handle = 0;
        }
    }
}

)";
    return s;
}

}  // namespace pvt

OSL_NAMESPACE_END
