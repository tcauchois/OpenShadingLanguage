// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include "vulkangridrender.h"

#include <algorithm>
#include <functional>
#include <map>
#include <sstream>
#include <cstring>

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/texture.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/sysutil.h>

#include <OSL/genclosure.h>
#include <OSL/oslclosure.h>
#include <OSL/journal.h>

#include "oslspirv.h"
#include "../liboslexec/spirv_abi.h"

namespace spirv_abi = OSL::pvt::spirv_abi;
#include "vkcompute.h"


// Hashes of the strings that SimpleRenderer's services know about.
namespace RS {
namespace {
namespace Hashes {
#define RS_STRDECL(str, var_name) \
    constexpr OSL::ustringhash var_name(OSL::strhash(str));
#include "rs_strdecls.h"
#undef RS_STRDECL
};  // namespace Hashes
}  // unnamed namespace
};  // namespace RS


OSL_NAMESPACE_BEGIN

namespace {

// Workgroup size of the generated kernel
const int tile = 8;

// Storage buffer bindings of the generated kernel
enum { OutputBuffer = 0, MessageBuffer, MatrixBuffer, ParamsBuffer };

// The message buffer: a word count, an overflow flag, then records of
//   { kind, shade_index, sequence, format (2 words), filename (2 words),
//     nargs, types[nargs], nwords, words[nwords] }
const size_t message_buffer_words = 1 << 20;
const int message_header_words    = 2;

// Message record kind for formatted closures (after the message kinds).
const uint32_t ClosureRecord = 4;

// Transformation handles for the ShaderGlobals, resolved by the kernel.
enum : uint64_t { ShaderHandle = 1, ObjectHandle = 2 };

// Most matrices (named and camera spaces) the kernel can look up.
const int max_matrices = 16;

using OIIO::Strutil::fmt::format;

}  // namespace



VulkanGridRenderer::VulkanGridRenderer()
    : m_vk(new spirv::VulkanCompute)
{
}



VulkanGridRenderer::~VulkanGridRenderer() {}



void
VulkanGridRenderer::clear()
{
    SimpleRenderer::clear();
    m_ready = false;
}



void
VulkanGridRenderer::prepare_render()
{
    std::string err;
    if (!m_vk->init(err)) {
        errhandler().errorfmt("Vulkan: {}", err);
        return;
    }
    // Nothing to do (like the CPU path) for an empty or unloadable group.
    int nlayers = 0;
    if (shaders().empty()
        || !shadingsys->getattribute(shaders()[0].get(), "num_layers",
                                     nlayers)
        || nlayers == 0)
        return;
    m_ready = build_pipeline();
    if (!m_ready) {
        // Don't write misleading (black) images for a shader we couldn't
        // run; testshade's regression tests rely on the exit status.
        errhandler().errorfmt("Vulkan: unable to run the shader group");
        exit(EXIT_FAILURE);
    }
}



// The matrices the kernel can look up, as { name hash (2 words), 16 floats
// of the matrix, 16 floats of its inverse }, preceded by their count. The
// handles for "shader" and "object" space use names 1 and 2, which are
// never string hashes.
std::vector<uint8_t>
VulkanGridRenderer::matrix_table() const
{
    std::vector<std::pair<uint64_t, Matrix44>> matrices;
    matrices.emplace_back(ShaderHandle, m_grid.shader2common);
    matrices.emplace_back(ObjectHandle, m_grid.object2common);
    for (auto&& x : m_named_xforms)
        matrices.emplace_back(x.first.hash(), *x.second);
    // Camera spaces are given as common-to-space; store them inverted so
    // that every entry is space-to-common.
    for (ustringhash space : { Hashes::camera, Hashes::screen, Hashes::NDC,
                               RS::Hashes::raster }) {
        Matrix44 m;
        if (const_cast<VulkanGridRenderer*>(this)->get_inverse_matrix(nullptr,
                                                                     m, space,
                                                                     0.0f))
            matrices.emplace_back(space.hash(), m.inverse());
    }

    if (matrices.size() > size_t(max_matrices)) {
        errhandler().warningfmt("Vulkan: only {} named spaces are supported",
                                max_matrices);
        matrices.resize(max_matrices);
    }
    std::vector<uint32_t> words = { uint32_t(matrices.size()) };
    for (auto&& m : matrices) {
        words.push_back(uint32_t(m.first));
        words.push_back(uint32_t(m.first >> 32));
        Matrix44 inv = m.second.inverse();
        for (const Matrix44* mat : { &m.second, &inv })
            for (int i = 0; i < 16; ++i) {
                uint32_t w;
                memcpy(&w, &(*mat)[i / 4][i % 4], 4);
                words.push_back(w);
            }
    }
    std::vector<uint8_t> bytes(words.size() * 4);
    memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}



// Load the textures the group needs (that exist) onto the device, with the
// MIP levels that each file has, and generate the kernel code that finds
// them by name: their index, number of channels, and the file's wrap
// modes (for "default" wrap).
void
VulkanGridRenderer::load_textures(ShaderGroup* group)
{
    m_vk->clear_textures();
    m_texture_table.clear();
    int ntextures      = 0;
    ustring* textures  = nullptr;
    int unknown        = 0;
    shadingsys->getattribute(group, "num_textures_needed", ntextures);
    shadingsys->getattribute(group, "textures_needed", TypeDesc::PTR,
                             &textures);
    shadingsys->getattribute(group, "unknown_textures_needed", unknown);
    if (unknown)
        errhandler().warningfmt(
            "Vulkan: the shader group uses textures whose names aren't known "
            "until it runs, which aren't supported");
    for (int i = 0; i < ntextures; ++i) {
        ustring name = textures[i];
        OIIO::ImageBuf image;
        if (!image.init_spec(name, 0, 0)) {
            image.geterror();  // Missing: lookups get the missing color or
            continue;          // an error
        }
        const OIIO::ImageSpec& spec = image.spec();
        int width = spec.width, height = spec.height;
        int nchannels = std::min(spec.nchannels, 4);
        std::vector<std::vector<float>> levels;
        for (int l = 0; l < std::max(1, image.nmiplevels()); ++l) {
            OIIO::ImageBuf level(name, 0, l);
            int w = std::max(1, width >> l), h = std::max(1, height >> l);
            if (!level.read(0, l, true, TypeFloat) || level.spec().width != w
                || level.spec().height != h)
                break;
            std::vector<float> pixels(size_t(w) * h * spec.nchannels);
            level.get_pixels(level.roi(), TypeFloat, pixels.data());
            std::vector<float> rgba(size_t(w) * h * 4, 0.0f);
            for (size_t p = 0; p < size_t(w) * h; ++p)
                for (int c = 0; c < nchannels; ++c)
                    rgba[4 * p + c] = pixels[p * spec.nchannels + c];
            levels.push_back(std::move(rgba));
        }
        // Files without MIP levels (e.g., not made with maketx) get them
        // by averaging 2x2 blocks, which is all a texture system can do.
        while (image.nmiplevels() <= 1 && levels.size()
               && (std::max(1, width >> (levels.size() - 1)) > 1
                   || std::max(1, height >> (levels.size() - 1)) > 1)) {
            int l  = int(levels.size());
            int pw = std::max(1, width >> (l - 1));
            int ph = std::max(1, height >> (l - 1));
            int w = std::max(1, width >> l), h = std::max(1, height >> l);
            const std::vector<float>& prev = levels.back();
            std::vector<float> rgba(size_t(w) * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c) {
                        float sum = 0;
                        for (int j = 0; j < 2; ++j)
                            for (int i = 0; i < 2; ++i) {
                                int px = std::min(2 * x + i, pw - 1);
                                int py = std::min(2 * y + j, ph - 1);
                                sum += prev[(size_t(py) * pw + px) * 4 + c];
                            }
                        rgba[(size_t(y) * w + x) * 4 + c] = sum / 4;
                    }
            levels.push_back(std::move(rgba));
        }
        std::string err;
        int index = levels.empty()
                        ? -1
                        : m_vk->add_texture(width, height, levels, err);
        if (index < 0) {
            errhandler().warningfmt("Vulkan: could not load texture {}: {}",
                                    name, err);
            continue;
        }
        // Files without wrap modes are black outside, as OIIO does.
        TextureOpt::Wrap swrap = TextureOpt::WrapBlack;
        TextureOpt::Wrap twrap = TextureOpt::WrapBlack;
        std::string wrapmodes  = spec.get_string_attribute("wrapmodes");
        if (wrapmodes.size())
            TextureOpt::parse_wrapmodes(wrapmodes.c_str(), swrap, twrap);
        m_texture_table += format(
            "        if (filename == {}ull) {{ index = {}; texchans = {}; "
            "fswrap = {}; ftwrap = {}; }}\n",
            ustringhash(name).hash(), index, nchannels, int(swrap),
            int(twrap));
    }
}



// printf of a closure (for groups with closures): a message record of kind
// ClosureRecord, { the placeholder (2 words), the root handle, then the
// pool's words }, which journal_messages formats, walking the tree. (On
// the GPU, a closure walk is a long unrolled loop, because Slang 2026.8
// crashes on loops here; copying the pool is much cheaper to compile.)
static const char* closure_printer = R"(
[noinline] export void osl_spirv_print_closure(uint shade_index,
    uint64_t placeholder, uint root, inout OSLClosurePool pool)
{
    uint nwords = 3 + POOL_WORDS;
    uint size = 9 + nwords;
    uint start;
    InterlockedAdd(msgbuf[0], size, start);
    start += MESSAGE_HEADER_WORDS;
    if (start + size >= MESSAGE_BUFFER_WORDS) {
        msgbuf[1] = 1;  // overflow
        return;
    }
    // A record like a message's, so that it sorts with them.
    msgbuf[start + 0] = CLOSURE_RECORD;
    msgbuf[start + 1] = shade_index;
    msgbuf[start + 2] = g_message_sequence++;
    msgbuf[start + 3] = 0;
    msgbuf[start + 4] = 0;
    msgbuf[start + 5] = 0;
    msgbuf[start + 6] = 0;
    msgbuf[start + 7] = 0;       // nargs
    msgbuf[start + 8] = nwords;
    msgbuf[start + 9] = uint(placeholder);
    msgbuf[start + 10] = uint(placeholder >> 32);
    msgbuf[start + 11] = root;
    [ForceUnroll] for (uint i = 0; i < POOL_WORDS; ++i)
        msgbuf[start + 12 + i] = pool.words[i];
}

uint osl_keep_closure_printer()
{
    OSLClosurePool pool = {};
    osl_spirv_print_closure(0, 0, 0, pool);
    return pool.used;
}
)";



// The closure printer, for a pool of `pool_words` words.
std::string
VulkanGridRenderer::closure_printer_source(int pool_words) const
{
    std::string s = closure_printer;
    s = OIIO::Strutil::replace(s, "MESSAGE_BUFFER_WORDS",
                               format("{}u", message_buffer_words), true);
    s = OIIO::Strutil::replace(s, "MESSAGE_HEADER_WORDS",
                               format("{}u", message_header_words), true);
    s = OIIO::Strutil::replace(s, "CLOSURE_RECORD",
                               format("{}u", ClosureRecord), true);
    s = OIIO::Strutil::replace(s, "POOL_WORDS", format("{}u", pool_words),
                               true);
    return s;
}



// Generate the compute kernel: set up the shader globals for grid point
// (x,y) exactly as testshade's setup_shaderglobals() does, run the group,
// and write the renderer outputs to the output buffer as 32-bit words
// (floats, or the bits of ints). The kernel also implements the renderer
// callbacks that the shader group needs.
std::string
VulkanGridRenderer::kernel_source(const std::vector<std::string>& fields,
                                  const std::vector<TypeDesc>& types) const
{
    std::string writes;
    size_t offset = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
        int n = types[i].basevalues();
        for (int c = 0; c < n; ++c) {
            // Aggregates are Vec3 structs or flat float arrays
            std::string elem;
            if (types[i].aggregate == TypeDesc::VEC3)
                elem = format(".{}", "xyz"[c]);
            else if (types[i].aggregate == TypeDesc::MATRIX44)
                elem = format(".f{}", c);
            std::string v = format("gd.{}[0]{}", fields[i], elem);
            v = types[i].basetype == TypeDesc::INT ? "uint(" + v + ")"
                                                   : "asuint(" + v + ")";
            writes += format("    outbuf[base + {}] = {};\n", offset + c, v);
        }
        offset += n;
    }

    // Attributes that SimpleRenderer knows (except userdata based on the
    // shading point, which isn't supported yet).
    auto u64 = [](ustringhash h) { return format("{}ull", h.hash()); };
    // Floats as their exact bits.
    auto fbits = [](float f) { return format("{}u", OSL::bitcast<uint32_t>(f)); };
    const TypeDesc TypeInt2(TypeDesc::INT, 2), TypeFloat2(TypeDesc::FLOAT, 2),
        TypeFloat4(TypeDesc::FLOAT, 4);
    struct Attr {
        ustringhash name;
        TypeDesc type;
        std::vector<std::string> values;
    };
    std::vector<Attr> attrs = {
        { RS::Hashes::osl_version, TypeInt, { format("{}", OSL_VERSION) } },
        { RS::Hashes::camera_resolution,
          TypeInt2,
          { format("{}", m_xres), format("{}", m_yres) } },
        { RS::Hashes::camera_projection,
          TypeString,
          { format("{}", uint32_t(m_projection.hash())),
            format("{}", uint32_t(m_projection.hash() >> 32)) } },
        { RS::Hashes::camera_pixelaspect,
          TypeFloat,
          { fbits(m_pixelaspect) } },
        { RS::Hashes::camera_fov, TypeFloat, { fbits(m_fov) } },
        { RS::Hashes::camera_clip,
          TypeFloat2,
          { fbits(m_hither), fbits(m_yon) } },
        { RS::Hashes::camera_clip_near,
          TypeFloat,
          { fbits(m_hither) } },
        { RS::Hashes::camera_clip_far,
          TypeFloat,
          { fbits(m_yon) } },
        { RS::Hashes::camera_shutter,
          TypeFloat2,
          { fbits(m_shutter[0]),
            fbits(m_shutter[1]) } },
        { RS::Hashes::camera_shutter_open,
          TypeFloat,
          { fbits(m_shutter[0]) } },
        { RS::Hashes::camera_shutter_close,
          TypeFloat,
          { fbits(m_shutter[1]) } },
        { RS::Hashes::camera_screen_window,
          TypeFloat4,
          { fbits(m_screen_window[0]),
            fbits(m_screen_window[1]),
            fbits(m_screen_window[2]),
            fbits(m_screen_window[3]) } },
    };
    std::string attr_code;
    for (auto&& a : attrs) {
        // A float's derivatives follow its values, zeroed by the caller.
        std::string sets;
        for (size_t i = 0; i < a.values.size(); ++i)
            sets += format(" result.words[{}] = {};", i, a.values[i]);
        attr_code += format("        if (name == {} && type == {}ull) {{{} "
                            "return 1; }}\n",
                            u64(a.name), OSL::bitcast<uint64_t>(a.type), sets);
    }
    attr_code += format(
        "        if (object == {} && name == {} && type == {}ull) {{ "
        "result.words[0] = asuint(3.14159f); return 1; }}\n",
        u64(RS::Hashes::options), u64(RS::Hashes::blahblah),
        OSL::bitcast<uint64_t>(TypeFloat));
    attr_code += format(
        "        if (object == 0 && name == {} && type == {}ull) {{ "
        "result.words[0] = uint(g_shade_index); return 1; }}\n",
        u64(RS::Hashes::shading_index), OSL::bitcast<uint64_t>(TypeInt));

    // Userdata: SimpleRenderer's procedural values (from the shading point)
    // and the constant --userdata values.
    std::string udata_code;
    auto ffield = [&](ustringhash name, TypeDesc type, const char* cond,
                      const char* val, const char* dx, const char* dy) {
        udata_code += format(
            "        if (name == {} && type == {}ull{}) {{ "
            "result.words[0] = asuint({}); "
            "if (derivs != 0) {{ result.words[1] = asuint({}); "
            "result.words[2] = asuint({}); }} return 1; }}\n",
            u64(name), OSL::bitcast<uint64_t>(type), cond, val, dx, dy);
    };
    udata_code += format("        if (name == {} && type == {}ull) {{ "
                         "result.words[0] = uint(int(4 * g_u)); return 1; }}\n",
                         u64(RS::Hashes::face_idx),
                         OSL::bitcast<uint64_t>(TypeInt));
    ffield(RS::Hashes::s, TypeFloat, "", "g_u", "g_dudx", "g_dudy");
    ffield(RS::Hashes::t, TypeFloat, "", "g_v", "g_dvdx", "g_dvdy");
    ffield(RS::Hashes::red, TypeFloat, " && g_Px > 0.5f", "g_u", "g_dudx",
           "g_dudy");
    ffield(RS::Hashes::green, TypeFloat, " && g_Px < 0.5f", "g_v", "g_dvdx",
           "g_dvdy");
    ffield(RS::Hashes::blue, TypeFloat, " && (int(g_Py * 12) % 2) == 0",
           "1.0f - g_u", "-g_dudx", "-g_dudy");
    for (const OIIO::ParamValue& p : userdata) {
        // Constant values (as 32-bit words, or 64-bit string hashes), with
        // zero derivatives.
        TypeDesc t = p.type();
        std::vector<uint32_t> words;
        if (t.basetype == TypeDesc::STRING) {
            for (int i = 0; i < p.nvalues() * int(t.basevalues()); ++i) {
                uint64_t h = ustring(((const char**)p.data())[i]).hash();
                words.push_back(uint32_t(h));
                words.push_back(uint32_t(h >> 32));
            }
        } else {
            words.resize(t.size() / 4);
            memcpy(words.data(), p.data(), words.size() * 4);
        }
        // Derivatives, if requested, are zero.
        std::string sets;
        for (size_t i = 0; i < 16; ++i)
            sets += format(" result.words[{}] = {}u;", i,
                           i < words.size() ? words[i] : 0);
        udata_code += format("        if (name == {} && type == {}ull) {{{} "
                             "return 1; }}\n",
                             u64(ustringhash(p.name())),
                             OSL::bitcast<uint64_t>(t), sets);
    }

    const GridParams& g = m_grid;
    return format(R"(
#include "osl_abi.slang"

RWStructuredBuffer<uint> outbuf;
RWStructuredBuffer<uint> msgbuf;
RWStructuredBuffer<uint> matrices;
RWStructuredBuffer<uint> params;  // the group's interactive_params
[[vk::binding({19})]] Texture2D textures[{20}];
[[vk::binding({21})]] SamplerState samplers[{22}];
[[vk::push_constant]] cbuffer Params {{ uint xres; uint yres; uint never; }};

// Per-invocation state. (Named so as not to clash with callback parameters,
// which crashes Slang 2026.8.)
static int g_shade_index;
static uint g_message_sequence;
static float g_u, g_v, g_dudx, g_dudy, g_dvdx, g_dvdy, g_Px, g_Py;

// The sampler for OIIO wrap modes (with "default" meaning the file's) and
// interpolation mode, as VulkanCompute::sampler_index() numbers them.
uint sampler_wrap(uint wrap, uint file_wrap)
{{
    uint w = wrap == 0 ? file_wrap : wrap;
    return w == 1 ? 1 : w == 2 ? 2 : w == 4 ? 3 : 0;  // black, clamp, mirror
}}

float4 sample_texture(uint index, uint smp, float2 st, float2 dx, float2 dy)
{{
    return textures[NonUniformResourceIndex(index)].SampleGrad(
        samplers[NonUniformResourceIndex(smp)], st, dx, dy);
}}

// NOTE: Slang 2026.8 crashes compiling a kernel that calls an [__extern]
// function if a function it keeps for linking has a loop that isn't
// unrolled. So the callbacks use only fixed-bound, unrolled loops.
struct Renderer : IOSLRenderer
{{
    static uint get_matrix(uint which, uint64_t handle, uint64_t name,
                           float time, uint inverse, inout OSLMatrix result)
    {{
        uint64_t key = which == OSL_MATRIX_FROM_HANDLE ? handle : name;
        uint n = matrices[0];
        uint found = 0;
        [ForceUnroll] for (uint i = 0; i < {17}; ++i) {{
            uint e = 1 + i * 34;
            if (i < n && found == 0
                && (uint64_t(matrices[e + 1]) << 32 | matrices[e]) == key)
                found = e;
        }}
        if (found == 0)
            return 0;
        uint m = found + 2 + (inverse != 0 ? 16 : 0);
        [ForceUnroll] for (uint j = 0; j < 16; ++j)
            result.m[j] = asfloat(matrices[m + j]);
        return 1;
    }}

    static uint get_attribute(uint64_t object, uint64_t name, uint64_t type,
                              uint derivs, uint index, inout OSLAttribute result)
    {{
{12}        // Like SimpleRenderer, fall back to userdata.
        if (object == 0)
            return get_userdata(name, type, derivs, result);
        return 0;
    }}

    static uint get_userdata(uint64_t name, uint64_t type, uint derivs,
                             inout OSLAttribute result)
    {{
{18}        return 0;
    }}

    static uint get_params(uint offset, inout OSLAttribute result)
    {{
        [ForceUnroll] for (uint i = 0; i < 16; ++i)
            result.words[i] = params[offset / 4 + i];
        return 1;
    }}

    // Sample a texture at `npoints` points with a shared filter footprint.
    // OSL does the rest (see rs_spirv.cpp).
    static uint texture_sample(uint64_t filename, uint swrap, uint twrap,
                               uint interp, uint npoints,
                               inout OSLTextureSample ts)
    {{
        uint index = 0, texchans = 0, fswrap = 0, ftwrap = 0;
{23}        if (texchans == 0)
            return 0;
        uint smp = (sampler_wrap(swrap, fswrap) * 4
                    + sampler_wrap(twrap, ftwrap)) * 2
                   + (interp == 0 ? 1 : 0);  // closest
        float2 dx = float2(ts.dx[0], ts.dx[1]);
        float2 dy = float2(ts.dy[0], ts.dy[1]);
        [ForceUnroll] for (uint p = 0; p < 4; ++p) {{
            if (p < npoints) {{
                float4 c = sample_texture(
                    index, smp, float2(ts.st[2 * p], ts.st[2 * p + 1]), dx,
                    dy);
                [ForceUnroll] for (uint i = 0; i < 4; ++i)
                    ts.rgba[4 * p + i] = c[i];
            }}
        }}
        uint w, h;
        textures[NonUniformResourceIndex(index)].GetDimensions(w, h);
        ts.info[0] = texchans;
        ts.info[1] = w;
        ts.info[2] = h;
        return 1;
    }}

    static void message(uint kind, uint shade_index, uint64_t format,
                        uint64_t filename, uint nargs,
                        inout OSLMessageArgs args)
    {{
        // Count the argument words.
        uint nwords = 0;
        [ForceUnroll] for (uint i = 0; i < 64; ++i)
            if (i < nargs)
                // ustringhash, int64, double, uint64, pointer and
                // TypeDesc are 64 bits (see EncodedType)
                nwords += (args.types[i] == 1 || args.types[i] == 2
                           || args.types[i] == 5) ? 1 : 2;
        uint size = 8 + nargs + 1 + nwords;
        uint start;
        InterlockedAdd(msgbuf[0], size, start);
        start += {13};
        if (start + size >= {14}) {{
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
{24}
[shader("compute")][numthreads({0}, {0}, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{{
    if (never == 0xffffffffu)
        outbuf[0] = osl_keep_callbacks(){25};
    int x = int(tid.x), y = int(tid.y);
    if (x >= xres || y >= yres)
        return;
    g_shade_index = y * int(xres) + x;
    g_message_sequence = 0;
    const float uscale = asfloat({1}), vscale = asfloat({2});
    const float uoffset = asfloat({3}), voffset = asfloat({4});
    ShaderGlobals sg = {{}};
    sg.raytype = {5};
    sg.shade_index = g_shade_index;
    sg.shader2common = {15};
    sg.object2common = {16};
    float u, v, dudx = 0, dudy = 0, dvdx = 0, dvdy = 0;
    if ({6}) {{
        u = uscale * float(x + 0.5f) / xres + uoffset;
        v = vscale * float(y + 0.5f) / yres + voffset;
        dudx = uscale / xres;
        dvdy = vscale / yres;
    }} else {{
        u = uscale * ((xres == 1) ? 0.5f : float(x) / (xres - 1)) + uoffset;
        v = vscale * ((yres == 1) ? 0.5f : float(y) / (yres - 1)) + voffset;
        dudx = uscale / max(1, int(xres) - 1);
        dvdy = vscale / max(1, int(yres) - 1);
    }}
    if ({7}) {{ dudx = 1.0f - u; dudy = u; }}
    if ({8}) {{ dvdx = 1.0f - v; dvdy = v; }}
    sg.u = {{ u, dudx, dudy }};
    sg.v = {{ v, dvdx, dvdy }};
    g_u = u; g_v = v; g_dudx = dudx; g_dudy = dudy; g_dvdx = dvdx;
    g_dvdy = dvdy; g_Px = u; g_Py = v;  // P = (u, v, 1)
    sg.P[0] = {{ u, v, 1.0f }};
    if ({9}) {{
        sg.P[1] = {{ 1.0f - u, 1.0f - v, u * 0.5f }};
        sg.P[2] = {{ 1.0f - v, 1.0f - u, v * 0.5f }};
    }} else {{
        sg.P[1] = {{ uscale / max(1, int(xres) - 1), 0.0f, 0.0f }};
        sg.P[2] = {{ 0.0f, vscale / max(1, int(yres) - 1), 0.0f }};
    }}
    sg.dPdu = {{ 1.0f, 0.0f, 0.0f }};
    sg.dPdv = {{ 0.0f, 1.0f, 0.0f }};
    sg.N  = {{ 0.0f, 0.0f, 1.0f }};
    sg.Ng = {{ 0.0f, 0.0f, 1.0f }};
    sg.surfacearea = 1;

    Groupdata gd = {{}};
    eval_osl(sg, gd);

    uint base = uint(g_shade_index) * {10};
{11}}}
)",
                  tile, fbits(g.uscale), fbits(g.vscale), fbits(g.uoffset),
                  fbits(g.voffset),
                  g.raytype_bit, g.pixelcenters ? "true" : "false",
                  g.vary_udxdy ? "true" : "false",
                  g.vary_vdxdy ? "true" : "false",
                  g.vary_Pdxdy ? "true" : "false",
                  std::max(size_t(1), offset), writes, attr_code,
                  message_header_words, message_buffer_words,
                  uint64_t(ShaderHandle), uint64_t(ObjectHandle),
                  max_matrices, udata_code, spirv::VulkanCompute::max_buffers,
                  spirv::VulkanCompute::max_textures,
                  spirv::VulkanCompute::max_buffers + 1,
                  spirv::VulkanCompute::num_samplers, m_texture_table,
                  m_closures ? closure_printer_source(m_closure_pool_words)
                             : "",
                  m_closures ? " + osl_keep_closure_printer()" : "");
}



bool
VulkanGridRenderer::build_pipeline()
{
    if (shaders().empty())
        return false;
    ShaderGroup* group = shaders()[0].get();
    shadingsys->optimize_group(group, nullptr);

    std::vector<uint32_t> osl_spirv;
    std::string abi;
    std::vector<std::string> fields;
    shadingsys->getattribute(group, "spirv_compiled_version", TypeDesc::PTR,
                             &osl_spirv);
    shadingsys->getattribute(group, "spirv_slang_abi", TypeDesc::PTR, &abi);
    shadingsys->getattribute(group, "spirv_output_fields", TypeDesc::PTR,
                             &fields);
    if (osl_spirv.empty()) {
        errhandler().errorfmt("Vulkan: shader group did not compile to SPIR-V");
        return false;
    }

    // Match each output image to its groupdata field. Outputs the group
    // doesn't compute (e.g., optimized away) are left black.
    std::vector<std::string> used_fields;
    std::vector<TypeDesc> used_types;
    m_output_types.clear();
    m_output_offsets.clear();
    m_pixel_words = 0;
    for (size_t i = 0; i < noutputs(); ++i) {
        const OIIO::ImageSpec& spec = outputbuf(i)->spec();
        TypeDesc t(TypeDesc::BASETYPE(spec.format.basetype),
                   spec.nchannels == 3    ? TypeDesc::VEC3
                   : spec.nchannels == 16 ? TypeDesc::MATRIX44
                                          : TypeDesc::SCALAR);
        m_output_types.push_back(t);
        m_output_offsets.push_back(m_pixel_words);
        if (i < fields.size() && !fields[i].empty()) {
            used_fields.push_back(fields[i]);
            used_types.push_back(t);
            m_pixel_words += spec.nchannels;
        } else {
            m_output_offsets.back() = size_t(-1);
        }
    }

    load_textures(group);
    // The closure pool's size, if the group has closures.
    m_closures          = false;
    m_closure_pool_words = 0;
    size_t pool         = abi.find("struct OSLClosurePool");
    if (pool != std::string::npos) {
        size_t w = abi.find("words[", pool);
        m_closures           = true;
        m_closure_pool_words = atoi(abi.c_str() + w + 6);
    }

    // The ABI is included (rather than imported) so that the kernel can
    // instantiate its OSL_RENDERER_CALLBACKS macro.
    std::string source = kernel_source(used_fields, used_types);
    source = OIIO::Strutil::replace(source, "#include \"osl_abi.slang\"", abi);

    // For debugging, TESTSHADE_VULKAN_DUMP=prefix writes the kernel's Slang
    // source and the SPIR-V modules.
    std::string dump = OIIO::Sysutil::getenv("TESTSHADE_VULKAN_DUMP");
    if (dump.size()) {
        OIIO::Filesystem::write_text_file(dump + "_kernel.slang", source);
        std::string e;
        spirv::write_file(dump + "_osl.spv", osl_spirv, e);
    }

    std::string err;
    std::vector<uint32_t> kernel, linked;
    std::vector<std::string> exports = { "osl_spirv_get_matrix",
                                         "osl_spirv_get_attribute",
                                         "osl_spirv_message",
                                         "osl_spirv_get_userdata",
                                         "osl_spirv_get_params",
                                         "osl_spirv_texture_sample" };
    if (m_closures)
        exports.push_back("osl_spirv_print_closure");
    if (!spirv::VulkanCompute::compile_slang(source, {}, { "eval_osl" },
                                             kernel, err)
        || !spirv::make_exports(kernel, exports, err)
        || !spirv::link({ kernel, osl_spirv }, linked, err)
        || !spirv::validate(linked, false, err)
        || !m_vk->set_pipeline(linked, err)) {
        errhandler().errorfmt("Vulkan: {}", err);
        return false;
    }
    return true;
}



void
VulkanGridRenderer::render(int xres, int yres)
{
    m_xres = xres;
    m_yres = yres;
    if (!m_ready)
        return;
    uint32_t push[3] = { uint32_t(xres), uint32_t(yres), 0 };
    m_buffers.resize(4);
    m_buffers[OutputBuffer].assign(std::max(size_t(1), m_pixel_words) * xres
                                       * yres * 4,
                                   0);
    m_buffers[MessageBuffer].assign(message_buffer_words * 4, 0);
    m_buffers[MatrixBuffer] = matrix_table();
    // Interactive parameters, with their current values (ReParameter
    // updates the group's copy), padded for get_params' 16-word reads.
    int psize      = 0;
    uint8_t* pdata = nullptr;
    ShaderGroup* group = shaders()[0].get();
    shadingsys->getattribute(group, "interactive_params_size", psize);
    shadingsys->getattribute(group, "interactive_params", TypeDesc::PTR,
                             &pdata);
    m_buffers[ParamsBuffer].assign(OIIO::round_to_multiple_of_pow2(psize, 4)
                                       + 64,
                                   0);
    if (pdata)
        memcpy(m_buffers[ParamsBuffer].data(), pdata, psize);
    std::string err;
    if (!m_vk->dispatch((xres + tile - 1) / tile, (yres + tile - 1) / tile,
                        push, sizeof(push), m_buffers, err))
        errhandler().errorfmt("Vulkan: {}", err);
    // Keep each render's messages (testshade may render several times).
    m_messages.push_back(m_buffers[MessageBuffer]);
}



void
VulkanGridRenderer::journal_messages(void* journal_buffer)
{
    OSL::journal::Writer jw { journal_buffer };
    for (const std::vector<uint8_t>& messages : m_messages)
        journal_messages(jw, messages);
    m_messages.clear();
}



void
VulkanGridRenderer::journal_messages(OSL::journal::Writer& jw,
                                     const std::vector<uint8_t>& messages)
{
    const uint32_t* w = reinterpret_cast<const uint32_t*>(messages.data());
    size_t end = std::min(size_t(w[0]) + message_header_words,
                          message_buffer_words);

    // Order messages as the CPU would produce them: by shading point, then
    // in the order each point emitted them.
    std::vector<size_t> records;
    for (size_t r = message_header_words; r + 8 < end;) {
        records.push_back(r);
        uint32_t nargs  = w[r + 7];
        uint32_t nwords = w[r + 8 + nargs];
        r += 9 + nargs + nwords;
    }
    std::stable_sort(records.begin(), records.end(), [&](size_t a, size_t b) {
        return std::make_pair(w[a + 1], w[a + 2])
               < std::make_pair(w[b + 1], w[b + 2]);
    });

    // Formatted closures, by placeholder, to substitute in the printf
    // records that follow them.
    std::map<uint64_t, ustringhash> closures;
    for (size_t r : records) {
        uint32_t kind        = w[r];
        int shade_index      = int(w[r + 1]);
        if (kind == ClosureRecord) {
            uint64_t placeholder = uint64_t(w[r + 10]) << 32 | w[r + 9];
            closures[placeholder] = ustring(
                format_closure(&w[r + 12], w[r + 8] - 3, w[r + 11]));
            continue;
        }
        uint64_t format_hash = uint64_t(w[r + 4]) << 32 | w[r + 3];
        uint64_t file_hash   = uint64_t(w[r + 6]) << 32 | w[r + 5];
        uint32_t nargs       = w[r + 7];
        std::vector<EncodedType> types(nargs);
        for (uint32_t i = 0; i < nargs; ++i)
            types[i] = EncodedType(w[r + 8 + i]);
        uint32_t nwords = w[r + 8 + nargs];
        std::vector<uint8_t> values(nwords * 4);
        memcpy(values.data(), &w[r + 9 + nargs], values.size());
        ustringhash fmt(format_hash), file(file_hash);
        // Substitute formatted closures for their placeholders.
        for (uint32_t i = 0, off = 0; i < nargs;
             off += pvt::size_of_encoded_type(types[i]), ++i) {
            if (types[i] != EncodedType::kUstringHash)
                continue;
            uint64_t h;
            memcpy(&h, &values[off], 8);
            auto found = closures.find(h);
            if (found != closures.end()) {
                h = found->second.hash();
                memcpy(&values[off], &h, 8);
            }
        }
        switch (kind) {
        case spirv_abi::Error:
            jw.record_errorfmt(0, shade_index, fmt, nargs, types.data(),
                               values.size(), values.data());
            break;
        case spirv_abi::Warning:
            jw.record_warningfmt(100, 0, shade_index, fmt, nargs,
                                 types.data(), values.size(), values.data());
            break;
        case spirv_abi::Print:
            jw.record_printfmt(0, shade_index, fmt, nargs, types.data(),
                               values.size(), values.data());
            break;
        case spirv_abi::FilePrint:
            jw.record_filefmt(0, shade_index, file, fmt, nargs, types.data(),
                              values.size(), values.data());
            break;
        }
    }
    if (w[1])
        errhandler().warningfmt("Vulkan: some shader messages were dropped");
}



// Format the closure with handle `root` in a closure pool (`n` words) the
// way the shading system's print_closure does.
std::string
VulkanGridRenderer::format_closure(const uint32_t* words, size_t n,
                                   uint32_t root) const
{
    auto f = [&](size_t i) {
        return i < n ? OSL::bitcast<float>(words[i]) : 0.0f;
    };
    auto word = [&](size_t i) { return i < n ? words[i] : 0u; };
    std::ostringstream out;
    out.imbue(std::locale::classic());
    bool first = true;
    std::function<void(uint32_t, Color3)> walk = [&](uint32_t handle,
                                                     Color3 w) {
        if (handle == 0 || handle > n)
            return;
        size_t node = handle - 1;
        int id      = int(word(node));
        if (id == ClosureColor::MUL) {
            walk(word(node + 4),
                 w * Color3(f(node + 1), f(node + 2), f(node + 3)));
            return;
        }
        if (id == ClosureColor::ADD) {
            walk(word(node + 1), w);
            walk(word(node + 2), w);
            return;
        }
        const char* name           = nullptr;
        const ClosureParam* params = nullptr;
        if (!shadingsys->query_closure(&name, &id, &params))
            return;
        if (!first)
            out << "\n\t+ ";
        first = false;
        out << "(" << w.x * f(node + 1) << ", " << w.y * f(node + 2) << ", "
            << w.z * f(node + 3) << ") * " << name << " (";
        size_t base = node + 4;
        for (int i = 0; params[i].type != TypeDesc(); ++i) {
            const ClosureParam& p = params[i];
            if (i)
                out << ", ";
            if (p.key)
                out << "\"" << p.key << "\", ";
            TypeDesc e = p.type.elementtype();
            int count  = int(p.type.numelements());
            if (count > 1)
                out << "[";
            for (int j = 0; j < count; ++j) {
                if (j)
                    out << ", ";
                size_t at = base + (p.offset + j * e.size()) / 4;
                if (e == TypeDesc::INT)
                    out << int(word(at));
                else if (e == TypeDesc::FLOAT)
                    out << f(at);
                else if (e.aggregate == TypeDesc::VEC3)
                    out << "(" << f(at) << ", " << f(at + 1) << ", "
                        << f(at + 2) << ")";
                else if (e == TypeDesc::STRING)
                    out << "\""
                        << ustringhash::from_hash(uint64_t(word(at + 1)) << 32
                                                  | word(at))
                        << "\"";
            }
            if (count > 1)
                out << "]";
        }
        out << ")";
    };
    walk(root, Color3(1.0f));
    return out.str();
}



void
VulkanGridRenderer::finalize_pixel_buffer()
{
    if (m_buffers.empty())
        return;
    // Each output is float or int, both 32 bits, so copy the words.
    const uint32_t* result = reinterpret_cast<const uint32_t*>(
        m_buffers[OutputBuffer].data());
    for (size_t i = 0; i < noutputs(); ++i) {
        if (m_output_offsets[i] == size_t(-1))
            continue;
        OIIO::ImageBuf* buf = outputbuf(i);
        int nchans          = buf->spec().nchannels;
        std::vector<uint32_t> pixels(size_t(m_xres) * m_yres * nchans);
        for (size_t p = 0, n = size_t(m_xres) * m_yres; p < n; ++p)
            for (int c = 0; c < nchans; ++c)
                pixels[p * nchans + c]
                    = result[p * m_pixel_words + m_output_offsets[i] + c];
        TypeDesc format = m_output_types[i].basetype == TypeDesc::INT
                              ? TypeDesc::INT
                              : TypeDesc::FLOAT;
        buf->set_pixels(OIIO::ROI::All(), format, pixels.data());
    }

    // Print the outputs like testshade's CPU path does.
    if (m_grid.print_outputs) {
        for (int y = 0; y < m_yres; ++y)
            for (int x = 0; x < m_xres; ++x) {
                OIIO::Strutil::print("Pixel ({}, {}):\n", x, y);
                size_t p = size_t(y) * m_xres + x;
                for (size_t i = 0; i < noutputs(); ++i) {
                    if (m_output_offsets[i] == size_t(-1))
                        continue;
                    const uint32_t* w = result + p * m_pixel_words
                                        + m_output_offsets[i];
                    std::string name = i < m_grid.output_names.size()
                                           ? m_grid.output_names[i].string()
                                           : outputname(i).string();
                    OIIO::Strutil::print("  {} :", name);
                    for (int c = 0; c < outputbuf(i)->spec().nchannels; ++c) {
                        if (m_output_types[i].basetype == TypeDesc::INT)
                            OIIO::Strutil::print(" {}", int(w[c]));
                        else
                            OIIO::Strutil::print(
                                " {:g}", OSL::bitcast<float>(w[c]));
                    }
                    OIIO::Strutil::print("\n");
                }
            }
    }
}


OSL_NAMESPACE_END
