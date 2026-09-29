// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include "toyrenderer.h"

#include <algorithm>
#include <cstring>

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/timer.h>

#include <OSL/encodedtypes.h>
#include <OSL/genclosure.h>
#include <OSL/journal.h>

#include "oslspirv.h"
#include "../liboslexec/spirv_abi.h"

namespace spirv_abi = OSL::pvt::spirv_abi;
#include "vkcompute.h"


OSL_NAMESPACE_BEGIN

using OIIO::Strutil::fmt::format;

namespace {

// The closures osltoy-vk's fragment shader knows how to light (with the
// same layouts as testshade's).
enum ClosureIDs { EMISSION_ID = 1, DIFFUSE_ID, MICROFACET_ID };

struct EmptyParams {};
struct DiffuseParams {
    Vec3 N;
    ustring label;
};
struct MicrofacetParams {
    ustring dist;
    Vec3 N, U;
    float xalpha, yalpha, eta;
    int refract;
};

void
register_closures(ShadingSystem* ss)
{
    struct BuiltinClosure {
        const char* name;
        int id;
        ClosureParam params[16];
    };
    BuiltinClosure builtins[] = {
        { "emission", EMISSION_ID, { CLOSURE_FINISH_PARAM(EmptyParams) } },
        { "diffuse",
          DIFFUSE_ID,
          { CLOSURE_VECTOR_PARAM(DiffuseParams, N),
            CLOSURE_STRING_KEYPARAM(DiffuseParams, label, "label"),
            CLOSURE_FINISH_PARAM(DiffuseParams) } },
        { "microfacet",
          MICROFACET_ID,
          { CLOSURE_STRING_PARAM(MicrofacetParams, dist),
            CLOSURE_VECTOR_PARAM(MicrofacetParams, N),
            CLOSURE_VECTOR_PARAM(MicrofacetParams, U),
            CLOSURE_FLOAT_PARAM(MicrofacetParams, xalpha),
            CLOSURE_FLOAT_PARAM(MicrofacetParams, yalpha),
            CLOSURE_FLOAT_PARAM(MicrofacetParams, eta),
            CLOSURE_INT_PARAM(MicrofacetParams, refract),
            CLOSURE_FINISH_PARAM(MicrofacetParams) } },
    };
    for (const BuiltinClosure& b : builtins)
        ss->register_closure(b.name, b.id, b.params, nullptr, nullptr);
}



// The vertex shader: transforms the sphere, and passes its world-space
// position, normal, uv, and dPdu to the fragment shader.
const char* vertex_source = R"(
struct Uniforms {
    float4x4 view_proj;
    float4 camera_pos;    // xyz, time
    float4 light_dir;     // xyz (toward the light), intensity
    float4 light_color;   // rgb, display output (0 = Ci, 1 = output)
    float4 ambient;       // rgb, exposure
    float4 viewport;      // width, height, frame, unused
};
[[vk::binding(0)]] ConstantBuffer<Uniforms> U;

struct VSIn {
    float3 P : POSITION;
    float3 N : NORMAL;
    float2 uv : TEXCOORD0;
    float3 dPdu : TEXCOORD1;
};
struct VSOut {
    float4 pos : SV_Position;
    float3 P : POSITION;
    float3 N : NORMAL;
    float2 uv : TEXCOORD0;
    float3 dPdu : TEXCOORD1;
};

[shader("vertex")]
VSOut main(VSIn v)
{
    VSOut o;
    o.pos = mul(U.view_proj, float4(v.P, 1));
    o.P = v.P;
    o.N = v.N;
    o.uv = v.uv;
    o.dPdu = v.dPdu;
    return o;
}
)";

}  // namespace



ToyRenderer::ToyRenderer()
{
    m_shadingsys = new ShadingSystem(this);
    m_shadingsys->attribute("allow_shader_replacement", 1);
    ustring outputs[] = { ustring("Cout") };
    m_shadingsys->attribute("renderer_outputs", TypeDesc(TypeDesc::STRING, 1),
                            &outputs);
    // Messages decode their format strings on the host, so keep them.
    m_shadingsys->attribute("lazyerror", 0);
    register_closures(m_shadingsys);
}



ToyRenderer::~ToyRenderer() { delete m_shadingsys; }



// The fragment shader: set up the ShaderGlobals from the rasterized sphere,
// run the group, and light its closures (or show its display output).
std::string
ToyRenderer::fragment_template(const std::string& abi) const
{
    std::string outfield;
    {
        // The display output's groupdata field, if it isn't Ci.
        std::vector<std::string> fields;
        ShaderGroupRef group = m_group;
        m_shadingsys->getattribute(group.get(), "spirv_output_fields",
                                   TypeDesc::PTR, &fields);
        if (fields.size() && fields[0].size())
            outfield = fields[0];
    }
    std::string display_output
        = outfield.size()
              ? format("float3(gd.{0}[0].x, gd.{0}[0].y, gd.{0}[0].z)",
                       outfield)
              : "float3(0, 0, 0)";
    bool has_closures = abi.find("struct OSLClosurePool") != std::string::npos;

    std::string closures;
    if (has_closures) {
        closures = R"(
// Light the closures: a directional light, plus ambient light.
struct Lighting : IOSLClosureVisitor
{
    float3 N, V, L;
    float3 light, ambient;
    float3 result;
    [mutating] void component(uint id, float3 w, uint node,
                              OSLClosurePool pool)
    {
        // (An if chain rather than a switch: Slang 2026.8 crashes on a
        // switch in a function called through a generic interface here.)
        if (id == OSL_CLOSURE_emission) {
            result += w;
        } else if (id == OSL_CLOSURE_diffuse) {
            OSLClosure_diffuse c = osl_closure_diffuse(pool, node);
            float3 n = normalize(c.p0);
            result += w * (light * max(0, dot(n, L)) + ambient) / 3.14159265f;
        } else if (id == OSL_CLOSURE_microfacet) {
            // GGX reflection, with Schlick's Fresnel approximation for eta.
            // (Refraction isn't supported.)
            OSLClosure_microfacet c = osl_closure_microfacet(pool, node);
            if (c.p6 == 0) {
                float3 n = normalize(c.p1);
                float a = max(0.001f, c.p3);
                float3 H = normalize(V + L);
                float NL = max(0, dot(n, L)), NV = max(1e-4f, dot(n, V));
                float NH = max(0, dot(n, H)), VH = max(0, dot(V, H));
                float a2 = a * a;
                float d = NH * NH * (a2 - 1) + 1;
                float D = a2 / (3.14159265f * d * d);
                float k = a / 2;
                float G = NL / (NL * (1 - k) + k) * NV / (NV * (1 - k) + k);
                float f0 = (c.p5 - 1) / (c.p5 + 1);
                f0 *= f0;
                float F = f0 + (1 - f0) * pow(1 - VH, 5);
                result += w * light * D * G * F / (4 * NV);
                result += w * ambient * F * (1 - a) / 3.14159265f;
            }
        }
        // Other closures aren't lit.
    }
}
)";
    }

    return format(R"(
{0}

struct Uniforms {{
    float4x4 view_proj;
    float4 camera_pos;    // xyz, time
    float4 light_dir;     // xyz (toward the light), intensity
    float4 light_color;   // rgb, display output (0 = Ci, 1 = output)
    float4 ambient;       // rgb, exposure
    float4 viewport;      // width, height, frame, unused
}};
[[vk::binding(0)]] ConstantBuffer<Uniforms> U;
[[vk::binding(1)]] RWStructuredBuffer<uint> msgbuf;   // messages
[[vk::binding(2)]] RWStructuredBuffer<uint> params;   // interactive params

static int g_shade_index;
static uint g_message_sequence;
static const uint MESSAGE_BUFFER_WORDS = {1};

// NOTE: Slang 2026.8 crashes compiling a shader that calls an [__extern]
// function if a function it keeps for linking has a loop that isn't
// unrolled. So the callbacks use only fixed-bound, unrolled loops.
struct Renderer : IOSLRenderer
{{
    static uint get_matrix(uint which, uint64_t handle, uint64_t name,
                           float time, uint inverse, inout OSLMatrix result)
    {{
        // Object and shader space are world space ("common").
        [ForceUnroll] for (uint j = 0; j < 16; ++j)
            result.m[j] = (j % 5) == 0 ? 1.0f : 0.0f;
        return which == OSL_MATRIX_FROM_HANDLE ? 1 : 0;
    }}

    static uint get_attribute(uint64_t object, uint64_t name, uint64_t type,
                              uint derivs, uint index, inout OSLAttribute result)
    {{
        return 0;
    }}

    static uint get_userdata(uint64_t name, uint64_t type, uint derivs,
                             inout OSLAttribute result)
    {{
        return 0;
    }}

    static uint get_params(uint offset, inout OSLAttribute result)
    {{
        [ForceUnroll] for (uint i = 0; i < 16; ++i)
            result.words[i] = params[offset / 4 + i];
        return 1;
    }}

    static uint texture_sample(uint64_t filename, uint swrap, uint twrap,
                               uint interp, uint npoints,
                               inout OSLTextureSample ts)
    {{
        return 0;  // no textures (yet)
    }}

    static void message(uint kind, uint shade_index, uint64_t format,
                        uint64_t filename, uint nargs,
                        inout OSLMessageArgs args)
    {{
        // Only from the pixel under the center of the view, so the console
        // isn't flooded.
        if (shade_index != uint(U.viewport.y / 2) * uint(U.viewport.x)
                               + uint(U.viewport.x / 2))
            return;
        uint nwords = 0;
        [ForceUnroll] for (uint i = 0; i < 64; ++i)
            if (i < nargs)
                // int32, float, and uint32 are 32 bits (see EncodedType)
                nwords += (args.types[i] == 1 || args.types[i] == 2
                           || args.types[i] == 5) ? 1 : 2;
        uint size = 8 + nargs + 1 + nwords;
        uint start;
        InterlockedAdd(msgbuf[0], size, start);
        start += 2;
        if (start + size >= MESSAGE_BUFFER_WORDS) {{
            msgbuf[1] = 1;  // overflow
            return;
        }}
        msgbuf[start + 0] = kind;
        msgbuf[start + 1] = shade_index;
        msgbuf[start + 2] = g_message_sequence++;
        msgbuf[start + 3] = uint(format);
        msgbuf[start + 4] = uint(format >> 32);
        msgbuf[start + 5] = uint(filename);
        msgbuf[start + 6] = uint(filename >> 32);
        msgbuf[start + 7] = nargs;
        [ForceUnroll] for (uint i = 0; i < 64; ++i)
            if (i < nargs)
                msgbuf[start + 8 + i] = args.types[i];
        msgbuf[start + 8 + nargs] = nwords;
        [ForceUnroll] for (uint w = 0; w < 128; ++w)
            if (w < nwords)
                msgbuf[start + 9 + nargs + w] = args.words[w];
    }}
}}

OSL_RENDERER_CALLBACKS(Renderer)
{2}
{3}

struct PSIn {{
    float4 pos : SV_Position;
    float3 P : POSITION;
    float3 N : NORMAL;
    float2 uv : TEXCOORD0;
    float3 dPdu : TEXCOORD1;
}};

float3 to_srgb(float3 c)
{{
    c = max(c, 0);
    return select(c <= 0.0031308f, 12.92f * c,
                  1.055f * pow(c, 1.0f / 2.4f) - 0.055f);
}}

[shader("fragment")]
float4 main(PSIn i) : SV_Target
{{
    if (U.viewport.w == 1234567.0f)
        msgbuf[0] = osl_keep_callbacks(){4};
    g_shade_index = int(i.pos.y) * int(U.viewport.x) + int(i.pos.x);
    g_message_sequence = 0;

    float3 P = i.P, N = normalize(i.N);
    float3 V = normalize(U.camera_pos.xyz - P);
    ShaderGlobals sg = {{}};
    sg.P[0] = {{ P.x, P.y, P.z }};
    float3 dPdx = ddx(P), dPdy = ddy(P);
    sg.P[1] = {{ dPdx.x, dPdx.y, dPdx.z }};
    sg.P[2] = {{ dPdy.x, dPdy.y, dPdy.z }};
    sg.I[0] = {{ -V.x, -V.y, -V.z }};
    sg.N = {{ N.x, N.y, N.z }};
    sg.Ng = sg.N;
    sg.u = {{ i.uv.x, ddx(i.uv.x), ddy(i.uv.x) }};
    sg.v = {{ i.uv.y, ddx(i.uv.y), ddy(i.uv.y) }};
    float3 dPdv = cross(N, i.dPdu);
    sg.dPdu = {{ i.dPdu.x, i.dPdu.y, i.dPdu.z }};
    sg.dPdv = {{ dPdv.x, dPdv.y, dPdv.z }};
    sg.time = U.camera_pos.w;
    sg.raytype = 1;  // camera
    sg.shade_index = g_shade_index;
    sg.surfacearea = 1;
    sg.shader2common = 1;
    sg.object2common = 1;

    Groupdata gd = {{}};
    eval_osl(sg, gd);

    float3 color;
    if (U.light_color.w != 0) {{
        color = {5};
    }} else {{
{6}
    }}
    return float4(to_srgb(color * U.ambient.w), 1);
}}
)",
                  abi, message_buffer_words,
                  has_closures ? R"(
[noinline] export void osl_spirv_print_closure(uint shade_index,
    uint64_t placeholder, uint root, inout OSLClosurePool pool)
{
    // (Closures aren't printed.)
}
uint osl_keep_closure_printer()
{
    OSLClosurePool pool = {};
    osl_spirv_print_closure(0, 0, 0, pool);
    return pool.used;
}
)"
                               : "",
                  closures,
                  has_closures ? " + osl_keep_closure_printer()" : "",
                  display_output,
                  has_closures ? R"(        Lighting lighting;
        lighting.N = N;
        lighting.V = V;
        lighting.L = normalize(U.light_dir.xyz);
        lighting.light = U.light_color.rgb * U.light_dir.w;
        lighting.ambient = U.ambient.rgb;
        lighting.result = 0;
        osl_walk_closures(gd.closure_pool, uint(sg.Ci), lighting);
        color = lighting.result;)"
                               : format("        color = {};", display_output));
}



bool
ToyRenderer::build_program(Program& program, std::string& err)
{
    ShaderGroupRef group = shadergroup();
    if (!group) {
        err = "no shader group";
        return false;
    }
    BuildStats& st = program.stats;
    st             = BuildStats();
    OIIO::Timer timer;

    // Have OSL compile the group to SPIR-V.
    float opt0 = 0, llvm0 = 0, opt1 = 0, llvm1 = 0;
    m_shadingsys->getattribute("stat:optimization_time", opt0);
    m_shadingsys->getattribute("stat:total_llvm_time", llvm0);
    if (!m_display_output.empty()) {
        ustring outputs[] = { m_display_output };
        m_shadingsys->attribute(group.get(), "renderer_outputs",
                                TypeDesc(TypeDesc::STRING, 1), &outputs);
    }
    m_shadingsys->optimize_group(group.get(), nullptr);
    std::vector<uint32_t> osl_spirv;
    std::string abi;
    m_shadingsys->getattribute(group.get(), "spirv_compiled_version",
                               TypeDesc::PTR, &osl_spirv);
    m_shadingsys->getattribute(group.get(), "spirv_slang_abi", TypeDesc::PTR,
                               &abi);
    st.osl_to_spirv = timer.lap();
    m_shadingsys->getattribute("stat:optimization_time", opt1);
    m_shadingsys->getattribute("stat:total_llvm_time", llvm1);
    st.opt_time  = opt1 - opt0;
    st.llvm_time = llvm1 - llvm0;
    if (osl_spirv.empty()) {
        err = "the shader group did not compile to SPIR-V";
        return false;
    }
    st.osl_words = osl_spirv.size();

    int globals = 0;
    const ustring* names = nullptr;
    m_shadingsys->getattribute(group.get(), "num_globals_needed", globals);
    m_shadingsys->getattribute(group.get(), "globals_needed", TypeDesc::PTR,
                               &names);
    program.uses_time = false;
    for (int i = 0; i < globals; ++i)
        program.uses_time |= names[i] == "time";
    int psize = 0;
    m_shadingsys->getattribute(group.get(), "interactive_params_size", psize);
    program.params_bytes = size_t(psize);

    // Generate and compile the shaders.
    program.fragment_source = fragment_template(abi);
    // For debugging: OSLTOYVK_DUMP=prefix writes the shaders.
    std::string dump = OIIO::Sysutil::getenv("OSLTOYVK_DUMP");
    if (dump.size()) {
        OIIO::Filesystem::write_text_file(dump + "_frag.slang",
                                          program.fragment_source);
        std::string e;
        spirv::write_file(dump + "_osl.spv", osl_spirv, e);
    }
    timer.lap();
    std::vector<uint32_t> fragment;
    bool has_closures = abi.find("struct OSLClosurePool") != std::string::npos;
    if (!spirv::VulkanCompute::compile_slang(vertex_source, {}, {},
                                             program.vertex, err)
        || !spirv::VulkanCompute::compile_slang(program.fragment_source, {},
                                                { "eval_osl" }, fragment, err))
        return false;
    st.slang_compile = timer.lap();
    st.slang_words   = fragment.size();

    std::vector<std::string> exports = { "osl_spirv_get_matrix",
                                         "osl_spirv_get_attribute",
                                         "osl_spirv_message",
                                         "osl_spirv_get_userdata",
                                         "osl_spirv_get_params",
                                         "osl_spirv_texture_sample" };
    if (has_closures)
        exports.push_back("osl_spirv_print_closure");
    if (!spirv::make_exports(fragment, exports, err)
        || !spirv::link({ fragment, osl_spirv }, program.fragment, err)
        || !spirv::validate(program.fragment, false, err))
        return false;
    st.link_validate = timer.lap();
    st.linked_words  = program.fragment.size();

    spirv::ModuleStats ms;
    if (spirv::module_stats(program.fragment, ms, err)) {
        st.code_words         = ms.code_words;
        st.constant_words     = ms.constant_words;
        st.other_words        = ms.other_words;
        st.function_var_bytes = ms.function_variable_bytes;
        st.private_var_bytes  = ms.private_variable_bytes;
        for (auto&& f : ms.function_instructions)
            if (f.first == "eval_osl")
                st.eval_osl_instructions = f.second;
    }
    err.clear();
    int gdsize = 0;
    m_shadingsys->getattribute(group.get(), "llvm_groupdata_size", gdsize);
    st.groupdata_bytes = size_t(gdsize);
    size_t pool        = abi.find("struct OSLClosurePool");
    if (pool != std::string::npos)
        st.closure_pool_bytes
            = 4 * (1 + atoi(abi.c_str() + abi.find("words[", pool) + 6));
    return true;
}



std::vector<uint32_t>
ToyRenderer::interactive_params()
{
    ShaderGroupRef group = shadergroup();
    int psize            = 0;
    uint8_t* pdata       = nullptr;
    if (group) {
        m_shadingsys->getattribute(group.get(), "interactive_params_size",
                                   psize);
        m_shadingsys->getattribute(group.get(), "interactive_params",
                                   TypeDesc::PTR, &pdata);
    }
    std::vector<uint32_t> words((psize + 3) / 4 + 16, 0);
    if (pdata)
        memcpy(words.data(), pdata, psize);
    return words;
}



std::string
ToyRenderer::decode_messages(const uint32_t* w, size_t nwords,
                             size_t max_messages)
{
    if (nwords < 2)
        return {};
    // Record the messages in a journal, and have OSL's journal Reader
    // format them.
    struct TextReporter final : public journal::Reporter {
        std::string text;
        void report_error(int, int, const string_view& m) override
        {
            text += "ERROR: " + std::string(m);
        }
        void report_warning(int, int, const string_view& m) override
        {
            text += "WARNING: " + std::string(m);
        }
        void report_print(int, int, const string_view& m) override
        {
            text += std::string(m);
        }
        void report_file_print(int, int, const string_view& f,
                               const string_view& m) override
        {
            text += std::string(f) + ": " + std::string(m);
        }
    } reporter;
    std::vector<uint8_t> jbuffer(1 << 20);
    journal::initialize_buffer(jbuffer.data(), uint32_t(jbuffer.size()), 1024,
                               1);
    {
        journal::Writer jw { jbuffer.data() };
        size_t end   = std::min(size_t(w[0]) + 2, nwords);
        size_t count = 0;
        for (size_t r = 2; r + 8 < end && count < max_messages; ++count) {
            uint32_t kind  = w[r];
            int index      = int(w[r + 1]);
            ustringhash fmt(uint64_t(w[r + 4]) << 32 | w[r + 3]);
            ustringhash file(uint64_t(w[r + 6]) << 32 | w[r + 5]);
            uint32_t nargs = w[r + 7];
            std::vector<EncodedType> types(nargs);
            for (uint32_t i = 0; i < nargs; ++i)
                types[i] = EncodedType(w[r + 8 + i]);
            uint32_t vwords = w[r + 8 + nargs];
            std::vector<uint8_t> vbytes(vwords * 4);
            memcpy(vbytes.data(), &w[r + 9 + nargs], vbytes.size());
            uint8_t* values = vbytes.data();
            if (kind == spirv_abi::Error)
                jw.record_errorfmt(0, index, fmt, nargs, types.data(),
                                   vwords * 4, values);
            else if (kind == spirv_abi::Warning)
                jw.record_warningfmt(100, 0, index, fmt, nargs, types.data(),
                                     vwords * 4, values);
            else if (kind == spirv_abi::Print)
                jw.record_printfmt(0, index, fmt, nargs, types.data(),
                                   vwords * 4, values);
            else if (kind == spirv_abi::FilePrint)
                jw.record_filefmt(0, index, file, fmt, nargs, types.data(),
                                  vwords * 4, values);
            r += 9 + nargs + vwords;
        }
    }
    journal::Reader jr { jbuffer.data(), reporter };
    jr.process();
    if (w[1])
        reporter.text += "(some messages were dropped)\n";
    return reporter.text;
}

OSL_NAMESPACE_END
