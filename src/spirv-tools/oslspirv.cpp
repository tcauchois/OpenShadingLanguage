// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include "oslspirv.h"

#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/linker.hpp>
#include <spirv/unified1/spirv.hpp11>


OSL_NAMESPACE_BEGIN

namespace spirv {

namespace {

// What we need to know about a module to patch or link it, gathered by
// spvBinaryParse.
struct ModuleInfo {
    const uint32_t* base = nullptr;
    std::unordered_map<std::string, uint32_t> name_to_id;
    std::unordered_map<uint32_t, std::string> id_to_name;
    std::unordered_set<uint32_t> linkage_decorated;
    std::unordered_map<uint32_t, bool> function_has_body;
    std::vector<uint32_t> functions;  // in module order
    uint32_t current_function = 0;
    bool has_linkage_cap      = false;
    size_t capabilities_end   = 0;  // word offset after the last OpCapability
    size_t annotations_begin  = 0;  // word offset where decorations start
};



// Is this opcode in one of the logical layout sections that precede the
// annotation (decoration) section?
bool
precedes_annotations(spv::Op op)
{
    switch (op) {
    case spv::Op::OpCapability:
    case spv::Op::OpExtension:
    case spv::Op::OpExtInstImport:
    case spv::Op::OpMemoryModel:
    case spv::Op::OpEntryPoint:
    case spv::Op::OpExecutionMode:
    case spv::Op::OpExecutionModeId:
    case spv::Op::OpString:
    case spv::Op::OpSource:
    case spv::Op::OpSourceContinued:
    case spv::Op::OpSourceExtension:
    case spv::Op::OpName:
    case spv::Op::OpMemberName:
    case spv::Op::OpModuleProcessed: return true;
    default: return false;
    }
}



// Literal string operand `i` of a parsed instruction. SPIR-V strings are
// nul-terminated and packed little-endian, so on little-endian hosts the
// words can be read directly as chars.
std::string
string_operand(const spv_parsed_instruction_t* inst, int i)
{
    const spv_parsed_operand_t& op = inst->operands[i];
    const char* s = reinterpret_cast<const char*>(inst->words + op.offset);
    return std::string(s, strnlen(s, op.num_words * sizeof(uint32_t)));
}



spv_result_t
collect_instruction(void* user_data, const spv_parsed_instruction_t* inst)
{
    ModuleInfo& info    = *static_cast<ModuleInfo*>(user_data);
    const size_t offset = inst->words - info.base;
    const spv::Op op    = spv::Op(inst->opcode);
    if (!info.annotations_begin && !precedes_annotations(op))
        info.annotations_begin = offset;
    switch (op) {
    case spv::Op::OpCapability:
        if (spv::Capability(inst->words[1]) == spv::Capability::Linkage)
            info.has_linkage_cap = true;
        info.capabilities_end = offset + inst->num_words;
        break;
    case spv::Op::OpName: {
        std::string name = string_operand(inst, 1);
        info.name_to_id.emplace(name, inst->words[1]);  // keep the first
        info.id_to_name.emplace(inst->words[1], std::move(name));
        break;
    }
    case spv::Op::OpDecorate:
        if (spv::Decoration(inst->words[2])
            == spv::Decoration::LinkageAttributes)
            info.linkage_decorated.insert(inst->words[1]);
        break;
    case spv::Op::OpFunction:
        info.current_function                   = inst->result_id;
        info.function_has_body[inst->result_id] = false;
        info.functions.push_back(inst->result_id);
        break;
    case spv::Op::OpLabel:
        if (info.current_function)
            info.function_has_body[info.current_function] = true;
        break;
    case spv::Op::OpFunctionEnd: info.current_function = 0; break;
    default: break;
    }
    return SPV_SUCCESS;
}



bool
parse(const std::vector<uint32_t>& binary, ModuleInfo& info, std::string& err)
{
    spvtools::Context context(SPV_ENV_UNIVERSAL_1_6);
    spv_diagnostic diagnostic = nullptr;
    info.base                 = binary.data();
    spv_result_t result = spvBinaryParse(context.CContext(), &info,
                                         binary.data(), binary.size(), nullptr,
                                         collect_instruction, &diagnostic);
    if (result != SPV_SUCCESS) {
        err = diagnostic ? diagnostic->error : "could not parse SPIR-V binary";
        spvDiagnosticDestroy(diagnostic);
        return false;
    }
    if (!info.annotations_begin)
        info.annotations_begin = binary.size();
    return true;
}



spvtools::MessageConsumer
collect_messages(std::string& err)
{
    return [&err](spv_message_level_t, const char*,
                  const spv_position_t& position, const char* message) {
        if (!err.empty())
            err += "\n";
        err += std::to_string(position.index) + ": " + message;
    };
}



// Functions declared without a body and without a linkage decoration.
// Front ends emit these for `[__extern]` declarations.
std::vector<std::string>
unlinked_stubs(const ModuleInfo& info)
{
    std::vector<std::string> stubs;
    for (uint32_t id : info.functions) {
        if (!info.function_has_body.at(id)
            && !info.linkage_decorated.count(id)) {
            auto name = info.id_to_name.find(id);
            stubs.push_back(name != info.id_to_name.end()
                                ? name->second
                                : "%" + std::to_string(id));
        }
    }
    return stubs;
}



// First word of an instruction: word count and opcode.
uint32_t
opcode_word(spv::Op op, size_t nwords)
{
    return (uint32_t(nwords) << spv::WordCountShift) | uint32_t(op);
}

}  // namespace



// Decorate the named functions with LinkageAttributes of the given type.
// Imports must be bodiless declarations, and exports must have bodies.
static bool
set_linkage(std::vector<uint32_t>& binary,
            const std::vector<std::string>& names, spv::LinkageType type,
            std::string& err)
{
    const bool import = type == spv::LinkageType::Import;
    ModuleInfo info;
    if (!parse(binary, info, err))
        return false;

    std::vector<uint32_t> decorations;
    for (const std::string& name : names) {
        auto id = info.name_to_id.find(name);
        if (id == info.name_to_id.end()) {
            err = "no symbol named \"" + name + "\"";
            return false;
        }
        auto fn = info.function_has_body.find(id->second);
        if (fn == info.function_has_body.end()) {
            err = "\"" + name + "\" is not a function";
            return false;
        }
        if (info.linkage_decorated.count(id->second)) {
            err = "function \"" + name + "\" already has linkage";
            return false;
        }
        if (fn->second == import) {
            err = "function \"" + name + "\" "
                  + (import ? "has a body" : "has no body");
            return false;
        }
        // OpDecorate %id LinkageAttributes "name" Import|Export
        size_t start = decorations.size();
        decorations.push_back(0);  // opcode word, filled in below
        decorations.push_back(id->second);
        decorations.push_back(uint32_t(spv::Decoration::LinkageAttributes));
        size_t str_start = decorations.size();
        decorations.resize(str_start + name.size() / sizeof(uint32_t) + 1, 0);
        memcpy(&decorations[str_start], name.data(), name.size());
        decorations.push_back(uint32_t(type));
        decorations[start] = opcode_word(spv::Op::OpDecorate,
                                         decorations.size() - start);
    }

    if (decorations.empty())
        return true;

    // Insert later positions first so earlier offsets stay valid.
    binary.insert(binary.begin() + info.annotations_begin, decorations.begin(),
                  decorations.end());
    if (!info.has_linkage_cap) {
        const uint32_t cap[] = { opcode_word(spv::Op::OpCapability, 2),
                                 uint32_t(spv::Capability::Linkage) };
        binary.insert(binary.begin() + info.capabilities_end, std::begin(cap),
                      std::end(cap));
    }
    return true;
}



bool
fix_structured_merges(std::vector<uint32_t>& binary, std::string& err)
{
    // Find each instruction's word offset and opcode.
    struct Inst {
        size_t offset;
        uint16_t nwords;
        spv::Op op;
    };
    struct Collect {
        const uint32_t* base;
        std::vector<Inst> insts;
    } collect { binary.data(), {} };
    spvtools::Context context(SPV_ENV_UNIVERSAL_1_6);
    spv_diagnostic diagnostic = nullptr;
    auto fn = [](void* user, const spv_parsed_instruction_t* inst) {
        auto& c = *static_cast<Collect*>(user);
        c.insts.push_back({ size_t(inst->words - c.base), inst->num_words,
                            spv::Op(inst->opcode) });
        return SPV_SUCCESS;
    };
    if (spvBinaryParse(context.CContext(), &collect, binary.data(),
                       binary.size(), nullptr, fn, &diagnostic)
        != SPV_SUCCESS) {
        err = diagnostic ? diagnostic->error : "could not parse SPIR-V binary";
        spvDiagnosticDestroy(diagnostic);
        return false;
    }

    auto is_branch = [](spv::Op op) {
        return op == spv::Op::OpBranch || op == spv::Op::OpBranchConditional
               || op == spv::Op::OpSwitch;
    };
    std::vector<uint32_t> out(binary.begin(), binary.begin() + 5);  // header
    const auto& insts = collect.insts;
    for (size_t i = 0; i < insts.size(); ++i) {
        const Inst& in = insts[i];
        auto words     = [&](const Inst& x) {
            return std::make_pair(binary.begin() + x.offset,
                                  binary.begin() + x.offset + x.nwords);
        };
        if ((in.op == spv::Op::OpSelectionMerge
             || in.op == spv::Op::OpLoopMerge)
            && i + 1 < insts.size() && !is_branch(insts[i + 1].op)) {
            // Emit the instructions up to the branch, then the merge.
            size_t j = i + 1;
            while (j < insts.size() && !is_branch(insts[j].op)
                   && insts[j].op != spv::Op::OpLabel)
                ++j;
            for (size_t k = i + 1; k < j; ++k) {
                auto w = words(insts[k]);
                out.insert(out.end(), w.first, w.second);
            }
            auto w = words(in);
            out.insert(out.end(), w.first, w.second);
            i = j - 1;
            continue;
        }
        auto w = words(in);
        out.insert(out.end(), w.first, w.second);
    }
    binary = std::move(out);
    return true;
}



bool
make_imports(std::vector<uint32_t>& binary,
             const std::vector<std::string>& names, std::string& err)
{
    return set_linkage(binary, names, spv::LinkageType::Import, err);
}



bool
make_exports(std::vector<uint32_t>& binary,
             const std::vector<std::string>& names, std::string& err)
{
    return set_linkage(binary, names, spv::LinkageType::Export, err);
}



bool
link(const std::vector<std::vector<uint32_t>>& modules,
     std::vector<uint32_t>& linked, std::string& err)
{
    if (modules.empty()) {
        err = "no modules to link";
        return false;
    }
    // The linker would silently keep an unresolved stub alongside the
    // exported definition of the same name, so reject stubs up front.
    for (size_t i = 0; i < modules.size(); ++i) {
        ModuleInfo info;
        if (!parse(modules[i], info, err))
            return false;
        std::vector<std::string> stubs = unlinked_stubs(info);
        if (!stubs.empty()) {
            err = "module " + std::to_string(i)
                  + " declares function(s) without a body or linkage:";
            for (const std::string& s : stubs)
                err += " " + s;
            err += " (use make_imports / oslspvfix --import first)";
            return false;
        }
    }

    // Fix up the inputs' known backend problems.
    std::vector<std::vector<uint32_t>> fixed = modules;
    for (auto& m : fixed)
        if (!fix_structured_merges(m, err))
            return false;

    spvtools::Context context(SPV_ENV_UNIVERSAL_1_6);
    context.SetMessageConsumer(collect_messages(err));
    spvtools::LinkerOptions options;
    // Slang and LLVM default to different SPIR-V versions.
    options.SetUseHighestVersion(true);
    // Pointer parameters' pointee types can differ trivially between front
    // ends (e.g., Slang types array lengths as int, LLVM as uint).
    options.SetAllowPtrTypeMismatch(true);
    linked.clear();
    if (spvtools::Link(context, fixed, &linked, options) != SPV_SUCCESS) {
        if (err.empty())
            err = "SPIR-V link failed";
        return false;
    }
    return true;
}



bool
validate(const std::vector<uint32_t>& binary, bool allow_linkage,
         std::string& err)
{
    spvtools::SpirvTools tools(allow_linkage ? SPV_ENV_UNIVERSAL_1_6
                                             : SPV_ENV_VULKAN_1_3);
    tools.SetMessageConsumer(collect_messages(err));
    return tools.Validate(binary);
}



bool
has_linkage(const std::vector<uint32_t>& binary)
{
    ModuleInfo info;
    std::string err;
    return parse(binary, info, err) && info.has_linkage_cap;
}



bool
module_stats(const std::vector<uint32_t>& binary, ModuleStats& stats,
             std::string& err)
{
    struct State {
        ModuleStats* stats;
        std::unordered_map<uint32_t, size_t> type_bytes;  // by type id
        std::unordered_map<uint32_t, uint32_t> pointee;   // pointer type id
        std::unordered_map<uint32_t, uint64_t> constants;  // integer values
        std::unordered_map<uint32_t, std::string> names;
        uint32_t function = 0;
        size_t function_instructions = 0;
    } state;
    stats       = ModuleStats();
    state.stats = &stats;
    stats.total_words = binary.size();

    auto fn = [](void* user, const spv_parsed_instruction_t* inst) {
        State& st       = *static_cast<State*>(user);
        ModuleStats& ms = *st.stats;
        const spv::Op op = spv::Op(inst->opcode);
        const uint32_t* w = inst->words;
        auto bytes = [&](uint32_t type) {
            auto found = st.type_bytes.find(type);
            return found == st.type_bytes.end() ? size_t(0) : found->second;
        };
        switch (op) {
        case spv::Op::OpName: st.names[w[1]] = string_operand(inst, 1); break;
        case spv::Op::OpTypeBool: st.type_bytes[w[1]] = 4; break;
        case spv::Op::OpTypeInt:
        case spv::Op::OpTypeFloat: st.type_bytes[w[1]] = w[2] / 8; break;
        case spv::Op::OpTypeVector:
        case spv::Op::OpTypeMatrix:
            st.type_bytes[w[1]] = bytes(w[2]) * w[3];
            break;
        case spv::Op::OpTypeArray: {
            auto n = st.constants.find(w[3]);
            st.type_bytes[w[1]] = bytes(w[2])
                                  * (n == st.constants.end() ? 0 : n->second);
            break;
        }
        case spv::Op::OpTypeStruct: {
            size_t b = 0;
            for (uint16_t i = 2; i < inst->num_words; ++i)
                b += bytes(w[i]);
            st.type_bytes[w[1]] = b;
            break;
        }
        case spv::Op::OpTypePointer: st.pointee[w[1]] = w[3]; break;
        case spv::Op::OpConstant:
            if (inst->num_words >= 4)
                st.constants[w[2]] = w[3];
            break;
        case spv::Op::OpVariable: {
            auto pt = st.pointee.find(w[1]);
            size_t b = pt == st.pointee.end() ? 0 : bytes(pt->second);
            if (spv::StorageClass(w[3]) == spv::StorageClass::Function)
                ms.function_variable_bytes += b;
            else if (spv::StorageClass(w[3]) == spv::StorageClass::Private)
                ms.private_variable_bytes += b;
            break;
        }
        case spv::Op::OpFunction:
            st.function              = inst->result_id;
            st.function_instructions = 0;
            break;
        default: break;
        }
        switch (op) {
        case spv::Op::OpConstant:
        case spv::Op::OpConstantComposite:
        case spv::Op::OpConstantTrue:
        case spv::Op::OpConstantFalse:
        case spv::Op::OpConstantNull:
        case spv::Op::OpSpecConstant:
        case spv::Op::OpSpecConstantComposite:
        case spv::Op::OpSpecConstantTrue:
        case spv::Op::OpSpecConstantFalse:
        case spv::Op::OpSpecConstantOp:
            ms.constant_words += inst->num_words;
            break;
        default:
            if (st.function)
                ms.code_words += inst->num_words;
            else
                ms.other_words += inst->num_words;
        }
        if (st.function)
            ++st.function_instructions;
        if (op == spv::Op::OpFunctionEnd) {
            auto name = st.names.find(st.function);
            ms.function_instructions.emplace_back(
                name != st.names.end() ? name->second
                                       : "%" + std::to_string(st.function),
                st.function_instructions);
            st.function = 0;
        }
        return SPV_SUCCESS;
    };
    spvtools::Context context(SPV_ENV_UNIVERSAL_1_6);
    spv_diagnostic diagnostic = nullptr;
    if (binary.size() < 5) {
        err = "SPIR-V binary too short";
        return false;
    }
    spv_result_t result = spvBinaryParse(context.CContext(), &state,
                                         binary.data(), binary.size(), nullptr,
                                         fn, &diagnostic);
    if (result != SPV_SUCCESS) {
        err = diagnostic ? diagnostic->error : "could not parse SPIR-V binary";
        spvDiagnosticDestroy(diagnostic);
        return false;
    }
    stats.other_words += 5;  // the header
    return true;
}



bool
read_file(const std::string& filename, std::vector<uint32_t>& binary,
          std::string& err)
{
    std::ifstream in(filename, std::ios::binary | std::ios::ate);
    if (!in) {
        err = "could not open \"" + filename + "\"";
        return false;
    }
    std::streamsize size = in.tellg();
    if (size % sizeof(uint32_t)) {
        err = "\"" + filename
              + "\" is not a SPIR-V binary (size not a multiple of 4)";
        return false;
    }
    in.seekg(0);
    binary.resize(size / sizeof(uint32_t));
    if (!in.read(reinterpret_cast<char*>(binary.data()), size)) {
        err = "could not read \"" + filename + "\"";
        return false;
    }
    return true;
}



bool
write_file(const std::string& filename, const std::vector<uint32_t>& binary,
           std::string& err)
{
    std::ofstream out(filename, std::ios::binary);
    if (!out
        || !out.write(reinterpret_cast<const char*>(binary.data()),
                      binary.size() * sizeof(uint32_t))) {
        err = "could not write \"" + filename + "\"";
        return false;
    }
    return true;
}

}  // namespace spirv

OSL_NAMESPACE_END
