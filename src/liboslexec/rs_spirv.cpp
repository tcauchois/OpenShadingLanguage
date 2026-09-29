// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// Renderer services for shader groups compiled to SPIR-V.
//
// This file is compiled only into the SPIR-V shadeops (with
// OSL_SPIRV_SHADEOPS). It implements the free-function renderer services
// (rs_*) and a few shadeops that the CPU implements in the shading system,
// in terms of a small set of functions that the renderer's shader supplies:
//
//   osl_spirv_get_matrix(which, handle, name, time, inverse, out matrix)
//   osl_spirv_get_attribute(object, name, typedesc, derivs, index,
//                           out values[16]) -> bool
//   osl_spirv_message(kind, shade_index, format, filename, nargs, types,
//                     values)
//   osl_spirv_get_userdata(name, typedesc, derivs, out values[16]) -> bool
//   osl_spirv_get_params(offset, out values[16]) -> bool
//   osl_spirv_texture_sample(filename, swrap, twrap, interp, npoints,
//                            inout sample) -> bool
//   osl_spirv_print_closure(shade_index, placeholder, root, inout pool)
//                     (called from generated code; see llvm_gen.cpp)
//
// These take only values and structs (logical addressing SPIR-V can't pass
// arbitrary pointers between modules), and their Slang declarations are part
// of the ABI that the shading system generates for each group.

#include <OpenImageIO/fmath.h>

#include <OSL/encodedtypes.h>
#include <OSL/fmt_util.h>
#include <OSL/oslconfig.h>
#include <OSL/rs_free_function.h>

#include "oslexec_pvt.h"
#include "spirv_abi.h"


using namespace OSL;
// The ABI: sizes, structs and codes shared with the shading system.
using namespace OSL::pvt::spirv_abi;


// Supplied by the renderer's shader.
extern "C" {
uint32_t
osl_spirv_get_matrix(uint32_t which, uint64_t handle, uint64_t name,
                     float time, uint32_t inverse, SpirvMatrix* result);
uint32_t
osl_spirv_get_attribute(uint64_t object, uint64_t name, uint64_t type,
                        uint32_t derivs, int32_t index,
                        SpirvAttribute* result);
void
osl_spirv_message(uint32_t kind, int32_t shade_index, uint64_t format,
                  uint64_t filename, int32_t nargs,
                  const SpirvMessageArgs* args);
uint32_t
osl_spirv_get_userdata(uint64_t name, uint64_t type, uint32_t derivs,
                       SpirvAttribute* result);
uint32_t
osl_spirv_texture_sample(uint64_t filename, uint32_t swrap, uint32_t twrap,
                         uint32_t interp, uint32_t npoints,
                         SpirvTextureSample* sample);
}


namespace {

bool
get_matrix(uint32_t which, uint64_t handle, uint64_t name, float time,
           bool inverse, Matrix44& result)
{
    SpirvMatrix m;
    bool ok = osl_spirv_get_matrix(which, handle, name, time, inverse, &m);
    memcpy((void*)&result, m.m, sizeof(m.m));
    return ok;
}


void
message(uint32_t kind, OpaqueExecContextPtr oec, ustringhash filename,
        ustringhash format, int32_t count, const EncodedType* types,
        uint32_t values_size, const uint8_t* values)
{
    // Messages that don't fit are replaced by a note saying so.
    SpirvMessageArgs args;
    if (count > max_message_args || values_size > 4 * max_message_words) {
        static constexpr char too_big[]
            = "(message too long for SPIR-V; format was \"{}\")";
        args.types[0] = uint32_t(EncodedType::kUstringHash);
        uint64_t f    = format.hash();
        memcpy(&args.words[0], &f, sizeof(f));
        osl_spirv_message(kind, get_shade_index(oec),
                          pvt::fmt_specification_hash(too_big).hash(),
                          filename.hash(), 1, &args);
        return;
    }
    // Fixed-bound, fully unrolled copies: once inlined into a shader, the
    // count and size are constants and the unneeded copies fold away. (A
    // runtime loop here would crash LLVM 22's SPIR-V backend.)
#pragma clang loop unroll(full)
    for (int i = 0; i < max_message_args; ++i)
        if (i < count)
            args.types[i] = uint32_t(types[i]);
#pragma clang loop unroll(full)
    for (uint32_t w = 0; w < uint32_t(max_message_words); ++w)
        if (w < values_size / 4)
            memcpy(&args.words[w], values + 4 * w, 4);
    osl_spirv_message(kind, get_shade_index(oec), format.hash(),
                      filename.hash(), count, &args);
}


// Texture wrap codes (OIIO's TextureOpt::Wrap), and interpolation.
enum : uint32_t { WrapPeriodic = 3, WrapClamp = 2 };

// Latlong environment coordinates for direction R, with +y up, as OIIO maps
// them.
void
latlong(const float* R, float* st)
{
    float len = sqrtf(R[2] * R[2] + R[0] * R[0]);
    st[0]     = OIIO::fast_atan2(-R[0], R[2]) / (2.0f * float(M_PI)) + 0.5f;
    st[1]     = 0.5f - OIIO::fast_atan2(R[1], len) / float(M_PI);
}


// Look up a texture, with the renderer only sampling it. Textures the
// renderer doesn't have (e.g., that weren't in the group's
// "textures_needed" list) give the missing color if there is one, or else
// an error. `coords` are s, t, dsdx, dtdx, dsdy, dtdy for a texture, or R,
// dRdx, dRdy for an environment.
bool
texture_lookup(bool environment, OpaqueExecContextPtr oec,
               ustringhash filename, const TextureOpt& options,
               const float* coords, int nchannels, float* result,
               float* dresultds, float* dresultdt, ustringhash* errormessage)
{
    SpirvTextureSample sample;
    float dx[2], dy[2];
    uint32_t swrap = uint32_t(options.swrap);
    uint32_t twrap = uint32_t(options.twrap);
    if (environment) {
        // Filter over the directions' (s,t) footprint.
        float st[2], stx[2], sty[2], R[3];
        latlong(coords, st);
        for (int i = 0; i < 3; ++i)
            R[i] = coords[i] + coords[3 + i];
        latlong(R, stx);
        for (int i = 0; i < 3; ++i)
            R[i] = coords[i] + coords[6 + i];
        latlong(R, sty);
        sample.st[0] = st[0];
        sample.st[1] = st[1];
        dx[0]        = stx[0] - st[0];
        dx[1]        = stx[1] - st[1];
        dy[0]        = sty[0] - st[0];
        dy[1]        = sty[1] - st[1];
        dx[0] -= roundf(dx[0]);  // across the seam
        dy[0] -= roundf(dy[0]);
        swrap = WrapPeriodic;
        twrap = WrapClamp;
    } else {
        sample.st[0] = coords[0];
        sample.st[1] = coords[1];
        dx[0]        = coords[2];
        dx[1]        = coords[3];
        dy[0]        = coords[4];
        dy[1]        = coords[5];
    }
    dx[0] *= options.swidth;
    dx[1] *= options.twidth;
    dy[0] *= options.swidth;
    dy[1] *= options.twidth;
    // Blur widens the filter by a fraction of the texture in s and t, as
    // OIIO adds it to the filter's axes.
    dx[0] += dx[0] < 0 ? -options.sblur : options.sblur;
    dy[1] += dy[1] < 0 ? -options.tblur : options.tblur;
    sample.dx[0] = dx[0];
    sample.dx[1] = dx[1];
    sample.dy[0] = dy[0];
    sample.dy[1] = dy[1];

    bool derivs = dresultds != nullptr;
    bool ok     = osl_spirv_texture_sample(filename.hash(), swrap, twrap,
                                       uint32_t(options.interpmode), 1,
                                       &sample);
    float rgba[4];
    for (int i = 0; i < 4; ++i)
        rgba[i] = ok ? sample.rgba[i] : 0.0f;
    // Derivatives with respect to s and t, by central differences over
    // the filter footprint (or a texel, if that's smaller).
    float cs[4] = { 0, 0, 0, 0 }, ct[4] = { 0, 0, 0, 0 };
    if (ok && derivs) {
        uint32_t width  = std::max(sample.info[1], 1u);
        uint32_t height = std::max(sample.info[2], 1u);
        float hs = std::max(0.5f * std::max(fabsf(dx[0]), fabsf(dy[0])),
                            0.5f / float(width));
        float ht = std::max(0.5f * std::max(fabsf(dx[1]), fabsf(dy[1])),
                            0.5f / float(height));
        float s0 = sample.st[0], t0 = sample.st[1];
        float offsets[8] = { s0 + hs, t0, s0 - hs, t0,
                             s0,      t0 + ht, s0, t0 - ht };
        for (int i = 0; i < 8; ++i)
            sample.st[i] = offsets[i];
        osl_spirv_texture_sample(filename.hash(), swrap, twrap,
                                 uint32_t(options.interpmode), 4, &sample);
#pragma clang loop unroll(full)
        for (int c = 0; c < 4; ++c) {
            cs[c] = (sample.rgba[c] - sample.rgba[4 + c]) / (2.0f * hs);
            ct[c] = (sample.rgba[8 + c] - sample.rgba[12 + c]) / (2.0f * ht);
        }
    }
    uint32_t texchans = sample.info[0];
    uint32_t first    = uint32_t(options.firstchannel);
    if (!ok && options.missingcolor) {
#pragma clang loop unroll(full)
        for (int i = 0; i < 4; ++i)
            rgba[i] = i < nchannels ? options.missingcolor[i] : 0.0f;
    }
#pragma clang loop unroll(full)
    for (int i = 0; i < 4; ++i) {
        if (i < nchannels) {
            // Channels past the texture's get the fill value.
            uint32_t ch = std::min(first + uint32_t(i), 3u);
            bool have   = ok && first + uint32_t(i) < texchans;
            if (ok)
                result[i] = have ? rgba[ch] : options.fill;
            else
                result[i] = options.missingcolor ? rgba[i] : 0.0f;
            if (derivs) {
                dresultds[i] = have ? cs[ch] : 0.0f;
                dresultdt[i] = have ? ct[ch] : 0.0f;
            }
        }
    }
    if (ok || options.missingcolor)
        return true;
    if (errormessage) {
        static constexpr char msg[] = "Texture not available to the renderer";
        *errormessage = pvt::fmt_specification_hash(msg);
    } else {
        OSL::errorfmt(oec, "Texture \"{}\" is not available to the renderer",
                      filename);
    }
    return false;
}

}  // namespace


// The transformation pointers in ShaderGlobals are opaque handles that the
// renderer assigns; pass them back to it.
OSL_RSOP OSL_HOSTDEVICE bool
rs_get_matrix_xform_time(OpaqueExecContextPtr, Matrix44& result,
                         TransformationPtr xform, float time)
{
    return get_matrix(MatrixFromHandle, uint64_t(xform), 0, time, false,
                      result);
}

OSL_RSOP OSL_HOSTDEVICE bool
rs_get_inverse_matrix_xform_time(OpaqueExecContextPtr, Matrix44& result,
                                 TransformationPtr xform, float time)
{
    return get_matrix(MatrixFromHandle, uint64_t(xform), 0, time, true,
                      result);
}

OSL_RSOP OSL_HOSTDEVICE bool
rs_get_matrix_space_time(OpaqueExecContextPtr, Matrix44& result,
                         ustringhash from, float time)
{
    return get_matrix(MatrixFromName, 0, from.hash(), time, false, result);
}

OSL_RSOP OSL_HOSTDEVICE bool
rs_get_inverse_matrix_space_time(OpaqueExecContextPtr, Matrix44& result,
                                 ustringhash to, float time)
{
    return get_matrix(MatrixFromName, 0, to.hash(), time, true, result);
}

OSL_RSOP OSL_HOSTDEVICE bool
rs_transform_points(OpaqueExecContextPtr, ustringhash, ustringhash, float,
                    const Vec3*, Vec3*, int, TypeDesc::VECSEMANTICS)
{
    // Only needed for nonlinear transformations, which we don't support.
    return false;
}


OSL_RSOP OSL_HOSTDEVICE void
rs_errorfmt(OpaqueExecContextPtr oec, ustringhash format, int32_t count,
            const EncodedType* types, uint32_t values_size, uint8_t* values)
{
    message(Error, oec, ustringhash(), format, count, types, values_size,
            values);
}

OSL_RSOP OSL_HOSTDEVICE void
rs_warningfmt(OpaqueExecContextPtr oec, ustringhash format, int32_t count,
              const EncodedType* types, uint32_t values_size, uint8_t* values)
{
    message(Warning, oec, ustringhash(), format, count, types, values_size,
            values);
}

OSL_RSOP OSL_HOSTDEVICE void
rs_printfmt(OpaqueExecContextPtr oec, ustringhash format, int32_t count,
            const EncodedType* types, uint32_t values_size, uint8_t* values)
{
    message(Print, oec, ustringhash(), format, count, types, values_size,
            values);
}

OSL_RSOP OSL_HOSTDEVICE void
rs_filefmt(OpaqueExecContextPtr oec, ustringhash filename, ustringhash format,
           int32_t count, const EncodedType* types, uint32_t values_size,
           uint8_t* values)
{
    message(FilePrint, oec, filename, format, count, types, values_size,
            values);
}


// Textures. The renderer can only supply textures that it has loaded, e.g.,
// the ones listed by the group's "textures_needed" attribute.

OSL_RSOP OSL_HOSTDEVICE bool
rs_texture(OpaqueExecContextPtr oec, ustringhash filename,
           TextureSystem::TextureHandle*, TextureSystem::Perthread*,
           TextureOpt& options, float s, float t, float dsdx, float dtdx,
           float dsdy, float dtdy, int nchannels, float* result,
           float* dresultds, float* dresultdt, ustringhash* errormessage)
{
    float coords[6] = { s, t, dsdx, dtdx, dsdy, dtdy };
    return texture_lookup(false, oec, filename, options, coords, nchannels,
                          result, dresultds, dresultdt, errormessage);
}

OSL_RSOP OSL_HOSTDEVICE bool
rs_environment(OpaqueExecContextPtr oec, ustringhash filename,
               TextureSystem::TextureHandle*, TextureSystem::Perthread*,
               TextureOpt& options, const Vec3& R, const Vec3& dRdx,
               const Vec3& dRdy, int nchannels, float* result,
               float* dresultds, float* dresultdt, ustringhash* errormessage)
{
    float coords[9] = { R.x,    R.y,    R.z,    dRdx.x, dRdx.y,
                        dRdx.z, dRdy.x, dRdy.y, dRdy.z };
    return texture_lookup(true, oec, filename, options, coords, nchannels,
                          result, dresultds, dresultdt, errormessage);
}


// Shadeops that the CPU implements in shadingsys.cpp, using the shading
// context or the virtual RendererServices.

OSL_SHADEOP int
osl_get_attribute(void* sg, int dest_derivs, ustringhash_pod obj_name,
                  ustringhash_pod attr_name, int array_lookup, int index,
                  long long attr_type, void* attr_dest)
{
    TypeDesc type = TYPEDESC(attr_type);
    SpirvAttribute result;
    bool ok = osl_spirv_get_attribute(obj_name, attr_name,
                                      uint64_t(attr_type), dest_derivs,
                                      array_lookup ? index : -1, &result);
    if (ok) {
        // Attribute values are 32-bit (float, int) or 64-bit (string hash).
        size_t basesize = type.basetype == TypeDesc::STRING ? 8 : 4;
        size_t size     = basesize * type.basevalues() * (dest_derivs ? 3 : 1);
        memcpy(attr_dest, result.words,
               std::min(size, sizeof(result.words)));
    }
    return ok;
}


// Userdata ("interpolated" parameters), with the same logic as the CPU's
// version in shadingsys.cpp: look it up the first time it's needed, then
// copy it to the parameter if it was found.
OSL_SHADEOP int
osl_bind_interpolated_param(void* sg, ustringhash_pod name, long long type,
                            int userdata_has_derivs, void* userdata_data,
                            int symbol_has_derivs, void* symbol_data,
                            int symbol_data_size, char* userdata_initialized,
                            int userdata_index)
{
    TypeDesc t    = TYPEDESC(type);
    size_t base   = t.basetype == TypeDesc::STRING ? 8 : 4;
    int udata_size = int(base * t.basevalues()) * (userdata_has_derivs ? 3 : 1);
    char status   = *userdata_initialized;
    if (status == 0) {
        SpirvAttribute result;
        bool ok = osl_spirv_get_userdata(name, uint64_t(type),
                                         userdata_has_derivs, &result);
        if (ok)
            memcpy(userdata_data, result.words,
                   std::min(size_t(udata_size), sizeof(result.words)));
        *userdata_initialized = status = 1 + ok;  // 1 = not found, 2 = found
    }
    if (status == 2) {
        memcpy(symbol_data, userdata_data,
               std::min(symbol_data_size, udata_size));
        return 1;
    }
    return 0;
}



// Debugging checks (the "debug_nan" and "debug_uninit" options), the same
// as the CPU's versions in shadingsys.cpp. The numbers of values checked
// are constants once these are inlined, so the loops unroll.

namespace {
// Not NaN or infinite. (The shadeops are compiled with -ffast-math, which
// assumes std::isfinite is always true.)
inline bool
is_finite(float x)
{
    return (OSL::bitcast<uint32_t>(x) & 0x7f800000u) != 0x7f800000u;
}
}  // namespace

OSL_SHADEOP void
osl_naninf_check(int ncomps, const void* vals_, int has_derivs, void* sg,
                 ustringhash_pod sourcefile, int sourceline,
                 ustringhash_pod symbolname, int firstcheck, int nchecks,
                 ustringhash_pod opname)
{
    const float* vals = (const float*)vals_;
    for (int d = 0; d < (has_derivs ? 3 : 1); ++d) {
        for (int c = firstcheck, e = c + nchecks; c < e; ++c) {
            int i = d * ncomps + c;
            if (!is_finite(vals[i])) {
                // (Strings as hashes: we can't make ustrings here.)
                static constexpr char derivs_of[] = "the derivatives of ";
                static constexpr char empty[]     = "";
                OSL::errorfmt(sg, "Detected {} value in {}{} at {}:{} (op {})",
                              vals[i],
                              d > 0 ? pvt::fmt_specification_hash(derivs_of)
                                    : pvt::fmt_specification_hash(empty),
                              ustringhash_from(symbolname),
                              ustringhash_from(sourcefile), sourceline,
                              ustringhash_from(opname));
                return;
            }
        }
    }
}


OSL_SHADEOP void
osl_uninit_check(long long typedesc_, void* vals_, void* sg,
                 ustringhash_pod sourcefile, int sourceline,
                 ustringhash_pod groupname_, int layer,
                 ustringhash_pod layername_, ustringhash_pod shadername,
                 int opnum, ustringhash_pod opname, int argnum,
                 ustringhash_pod symbolname, int firstcheck, int nchecks)
{
    TypeDesc typedesc = TYPEDESC(typedesc_);
    bool uninit       = false;
    if (typedesc.basetype == TypeDesc::FLOAT) {
        float* vals = (float*)vals_;
        for (int c = firstcheck, e = firstcheck + nchecks; c < e; ++c)
            if (!is_finite(vals[c])) {
                uninit  = true;
                vals[c] = 0;
            }
    }
    if (typedesc.basetype == TypeDesc::INT) {
        int* vals = (int*)vals_;
        for (int c = firstcheck, e = firstcheck + nchecks; c < e; ++c)
            if (vals[c] == std::numeric_limits<int>::min()) {
                uninit  = true;
                vals[c] = 0;
            }
    }
    if (typedesc.basetype == TypeDesc::STRING) {
        ustringhash* vals = (ustringhash*)vals_;
        for (int c = firstcheck, e = firstcheck + nchecks; c < e; ++c)
            if (vals[c] == Hashes::uninitialized_string) {
                uninit  = true;
                vals[c] = ustringhash();
            }
    }
    if (uninit) {
        static constexpr char unnamed_group[] = "<unnamed group>";
        static constexpr char unnamed_layer[] = "<unnamed layer>";
        ustringhash groupname = ustringhash_from(groupname_);
        ustringhash layername = ustringhash_from(layername_);
        OSL::errorfmt(
            sg,
            "Detected possible use of uninitialized value in {} {} at {}:{} (group {}, layer {} {}, shader {}, op {} '{}', arg {})",
            typedesc, ustringhash_from(symbolname),
            ustringhash_from(sourcefile), sourceline,
            groupname.empty() ? pvt::fmt_specification_hash(unnamed_group)
                              : groupname,
            layer,
            layername.empty() ? pvt::fmt_specification_hash(unnamed_layer)
                              : layername,
            ustringhash_from(shadername), opnum, ustringhash_from(opname),
            argnum);
    }
}



// Messages (setmessage/getmessage), with the same semantics and errors as
// the CPU's (opmessage.cpp).
//
// When every message name in the group is a constant (the usual case), the
// shading system gives each name its own "slot", an array of 32-bit words
// sized for that message, and calls the osl_spirv_*message_slot versions.
// Their accesses all have constant indices once inlined, so the slots can
// live in registers. Otherwise, messages go in a table that's searched by
// name. Both are globals, which the shading system turns into locals of
// the group's entry point: one per invocation, zeroed when it starts.

namespace {

constexpr int max_message_data = 64;  // 32-bit words: up to a float[64]

// Size in 32-bit words of a message's value. (TypeDesc::size() calls into
// OIIO, which isn't available here.) Values are made of 32-bit ints and
// floats, or 64-bit string hashes or closure handles (whose type is
// UNKNOWN, as the shading system passes them).
int
message_words(TypeDesc type)
{
    int base = type.basetype == TypeDesc::STRING
                       || type.basetype == TypeDesc::UNKNOWN
                   ? 2
                   : 1;
    return base * int(type.aggregate) * std::max(1, type.arraylen);
}

// Copy n (a constant after inlining) words between a message and a value.
inline void
copy_words(uint32_t* dst, const void* src, int n)
{
#pragma clang loop unroll(full)
    for (int w = 0; w < max_message_data; ++w)
        if (w < n)
            memcpy(&dst[w], (const char*)src + 4 * w, 4);
}

inline void
copy_words(void* dst, const uint32_t* src, int n)
{
#pragma clang loop unroll(full)
    for (int w = 0; w < max_message_data; ++w)
        if (w < n)
            memcpy((char*)dst + 4 * w, &src[w], 4);
}

// The checks and errors common to both kinds of storage, given a message's
// header words (laid out as MessageSlotWord) and data.

void
set_message(void* sg, MessageHeader* hdr, uint32_t* data, ustringhash_pod name,
            TypeDesc type, const void* val, int layeridx,
            ustringhash_pod sourcefile, int sourceline)
{
    if (hdr->state != NoMessage) {
        if (hdr->state == HasData) {
            OSL::errorfmt(
                sg,
                "message \"{}\" already exists (created here: {}:{})"
                " cannot set again from {}:{}",
                ustringhash_from(name), ustringhash_from(hdr->sourcefile),
                int(hdr->line), ustringhash_from(sourcefile), sourceline);
        } else {
            OSL::errorfmt(
                sg,
                "message \"{}\" was queried before being set (queried here: {}:{})"
                " setting it now ({}:{}) would lead to inconsistent results",
                ustringhash_from(name), ustringhash_from(hdr->sourcefile),
                int(hdr->line), ustringhash_from(sourcefile), sourceline);
        }
        return;
    }
    hdr->state      = val ? HasData : Queried;
    hdr->type       = OSL::bitcast<uint64_t>(type);
    hdr->layer      = uint32_t(layeridx);
    hdr->sourcefile = sourcefile;
    hdr->line       = uint32_t(sourceline);
    if (val)
        copy_words(data, val, message_words(type));
}

int
get_message(void* sg, MessageHeader* hdr, const uint32_t* data,
            ustringhash_pod source, ustringhash_pod name, TypeDesc type,
            void* val, int derivs, int layeridx, ustringhash_pod sourcefile,
            int sourceline, bool strict)
{
    // Messages from trace() come from the renderer, which isn't supported
    // yet.
    static constexpr char trace[] = "trace";
    if (ustringhash_from(source) == pvt::fmt_specification_hash(trace))
        return 0;

    if (hdr->state == NoMessage) {
        // Not found: record that, in case a later layer tries to set it.
        if (strict)
            set_message(sg, hdr, nullptr, name, type, nullptr, layeridx,
                        sourcefile, sourceline);
        return 0;
    }
    TypeDesc mtype = OSL::bitcast<TypeDesc>(hdr->type);
    if (mtype != type) {
        static constexpr char created[] = "created";
        static constexpr char queried[] = "queried";
        // Closures (whose type is UNKNOWN) print as "closure color". (A
        // TypeDesc and a string hash take the same space, but are encoded
        // differently, so these are separate messages.)
        static constexpr char closure[] = "closure color";
        ustringhash how = hdr->state == HasData
                              ? pvt::fmt_specification_hash(created)
                              : pvt::fmt_specification_hash(queried);
        if (mtype.basetype == TypeDesc::UNKNOWN)
            OSL::errorfmt(sg,
                          "type mismatch for message \"{}\" ({} as {} here: "
                          "{}:{}) cannot fetch as {} from {}:{}",
                          ustringhash_from(name), how,
                          pvt::fmt_specification_hash(closure),
                          ustringhash_from(hdr->sourcefile), int(hdr->line),
                          type, ustringhash_from(sourcefile), sourceline);
        else if (type.basetype == TypeDesc::UNKNOWN)
            OSL::errorfmt(sg,
                          "type mismatch for message \"{}\" ({} as {} here: "
                          "{}:{}) cannot fetch as {} from {}:{}",
                          ustringhash_from(name), how, mtype,
                          ustringhash_from(hdr->sourcefile), int(hdr->line),
                          pvt::fmt_specification_hash(closure),
                          ustringhash_from(sourcefile), sourceline);
        else
            OSL::errorfmt(sg,
                          "type mismatch for message \"{}\" ({} as {} here: "
                          "{}:{}) cannot fetch as {} from {}:{}",
                          ustringhash_from(name), how, mtype,
                          ustringhash_from(hdr->sourcefile), int(hdr->line),
                          type, ustringhash_from(sourcefile), sourceline);
        return 0;
    }
    if (hdr->state != HasData)
        return 0;
    if (int(hdr->layer) > layeridx) {
        OSL::errorfmt(sg,
                      "message \"{}\" was set by layer #{} ({}:{})"
                      " but is being queried by layer #{} ({}:{})"
                      " - messages may only be transferred from nodes "
                      "that appear earlier in the shading network",
                      ustringhash_from(name), int(hdr->layer),
                      ustringhash_from(hdr->sourcefile), int(hdr->line),
                      layeridx,
                      ustringhash_from(sourcefile), sourceline);
        return 0;
    }
    int nwords = message_words(type);
    copy_words(val, data, nwords);
    if (derivs) {  // derivatives aren't passed: zero them
#pragma clang loop unroll(full)
        for (int w = 0; w < 2 * max_message_data; ++w)
            if (w < 2 * nwords)
                memset((char*)val + 4 * (nwords + w), 0, 4);
    }
    return 1;
}


// The table searched by name, for groups whose message names aren't all
// known when they're compiled.

constexpr int max_messages = 32;

struct MessageTable {
    int count;
    uint64_t name[max_messages];
    MessageHeader header[max_messages];
    uint32_t data[max_messages][max_message_data];
};

MessageTable message_table;

// The index of the message with this name, adding it (empty) if there's
// room; or -1. (A fixed-bound loop, which unrolls: a runtime loop here
// would crash LLVM 22's SPIR-V backend.)
int
table_message(void* sg, uint64_t name, TypeDesc type,
              ustringhash_pod sourcefile, int sourceline)
{
    int found = -1;
#pragma clang loop unroll(full)
    for (int i = 0; i < max_messages; ++i)
        if (found < 0 && i < message_table.count
            && message_table.name[i] == name)
            found = i;
    if (found >= 0)
        return found;
    if (message_table.count >= max_messages
        || message_words(type) > max_message_data) {
        OSL::errorfmt(sg,
                      "message \"{}\" is too large, or there are too many "
                      "messages, for SPIR-V ({}:{})",
                      ustringhash_from(name), ustringhash_from(sourcefile),
                      sourceline);
        return -1;
    }
    found                     = message_table.count++;
    message_table.name[found] = name;
    message_table.header[found].state = NoMessage;
    return found;
}

}  // namespace


// Whether getmessage records messages that it didn't find (the
// "strict_messages" option). Set by the shading system for each group.
extern "C" const int osl_spirv_strict_messages;


// Messages in slots: each is a MessageHeader and its data words.

OSL_SHADEOP void
osl_spirv_setmessage_slot(ShaderGlobals* sg, MessageHeader* header,
                          uint32_t* data, ustringhash_pod name, long long type,
                          void* val, int layeridx, ustringhash_pod sourcefile,
                          int sourceline)
{
    set_message(sg, header, data, name, TYPEDESC(type), val, layeridx,
                sourcefile, sourceline);
}

OSL_SHADEOP int
osl_spirv_getmessage_slot(ShaderGlobals* sg, MessageHeader* header,
                          uint32_t* data, ustringhash_pod source,
                          ustringhash_pod name, long long type, void* val,
                          int derivs, int layeridx, ustringhash_pod sourcefile,
                          int sourceline)
{
    return get_message(sg, header, data, source, name, TYPEDESC(type), val,
                       derivs, layeridx, sourcefile, sourceline,
                       osl_spirv_strict_messages);
}


// Messages in the table.

OSL_SHADEOP void
osl_setmessage(ShaderGlobals* sg, ustringhash_pod name, long long type_,
               void* val, int layeridx, ustringhash_pod sourcefile,
               int sourceline)
{
    TypeDesc type = TYPEDESC(type_);
    int m         = table_message(sg, name, type, sourcefile, sourceline);
    if (m >= 0)
        set_message(sg, &message_table.header[m], message_table.data[m], name,
                    type, val, layeridx, sourcefile, sourceline);
}

OSL_SHADEOP int
osl_getmessage(ShaderGlobals* sg, ustringhash_pod source,
               ustringhash_pod name, long long type_, void* val, int derivs,
               int layeridx, ustringhash_pod sourcefile, int sourceline)
{
    TypeDesc type = TYPEDESC(type_);
    int m         = table_message(sg, name, type, sourcefile, sourceline);
    if (m < 0)
        return 0;
    return get_message(sg, &message_table.header[m], message_table.data[m],
                       source, name, type, val, derivs, layeridx, sourcefile,
                       sourceline, osl_spirv_strict_messages);
}



// Closures. A group builds its closures in a pool of 32-bit words in its
// groupdata, sized for all of its closure operations, and closure values
// are handles: 1 + the index of a node's first word, or 0 for no closure.
// The nodes (see spirv_abi.h) are
//
//   component: id, w (3 floats), the closure's params (host layout)
//   mul:       OSL_CLOSURE_MUL, w (3 floats), handle
//   add:       OSL_CLOSURE_ADD, handle, handle
//
// with the same semantics as opclosure.cpp's.

namespace {

// The pool is passed to these as its count of used words and its words
// (which are separate fields of the groupdata's OSLClosurePool, sized for
// the group), and `capacity`, the number of words.

// Allocate a node, returning its handle, or 0 if the pool is full (which
// can happen only for groups that build closures in loops).
uint64_t
allocate_closure_node(void* sg, uint32_t* used, int capacity, int nwords)
{
    if (int(*used) + nwords > capacity) {
        OSL::errorfmt(sg,
                      "Out of closure memory for SPIR-V (see the "
                      "\"spirv_closure_pool_words\" option)");
        return 0;
    }
    uint64_t handle = *used + 1;
    *used += nwords;
    return handle;
}

inline void
set_weight(uint32_t* words, uint32_t node, const Color3& w)
{
    memcpy(&words[node + 1], &w.x, 4);
    memcpy(&words[node + 2], &w.y, 4);
    memcpy(&words[node + 3], &w.z, 4);
}

}  // namespace


OSL_SHADEOP uint64_t
osl_spirv_allocate_closure_component(ShaderGlobals* sg, uint32_t* used,
                                     uint32_t* words, int capacity, int id,
                                     int nwords, const void* weight)
{
    Color3 w = weight ? *(const Color3*)weight : Color3(1.0f);
    // Zero weight: no closure.
    if (w.x == 0.0f && w.y == 0.0f && w.z == 0.0f)
        return 0;
    uint64_t handle = allocate_closure_node(
        sg, used, capacity, closure_component_header_words + nwords);
    if (handle) {
        uint32_t node = uint32_t(handle - 1);
        words[node]   = uint32_t(id);
        set_weight(words, node, w);
        // Params that aren't given are zero. (nwords is a constant after
        // inlining, so this unrolls.)
#pragma clang loop unroll(full)
        for (int i = 0; i < 128; ++i)
            if (i < nwords)
                words[node + closure_component_header_words + i] = 0;
    }
    return handle;
}


// Set (constant) `nwords` words of a component's params at `offset` words.
OSL_SHADEOP void
osl_spirv_closure_param(uint32_t* words, uint64_t handle, int offset,
                        const void* val, int nwords)
{
    if (!handle)
        return;
    uint32_t params = uint32_t(handle - 1) + closure_component_header_words
                      + offset;
#pragma clang loop unroll(full)
    for (int w = 0; w < 128; ++w)
        if (w < nwords)
            memcpy(&words[params + w], (const char*)val + 4 * w, 4);
}


OSL_SHADEOP uint64_t
osl_spirv_mul_closure_color(ShaderGlobals* sg, uint32_t* used,
                            uint32_t* words, int capacity, uint64_t a,
                            const void* weight)
{
    const Color3& w = *(const Color3*)weight;
    if (!a)
        return 0;
    if (w.x == 0.0f && w.y == 0.0f && w.z == 0.0f)
        return 0;
    if (w.x == 1.0f && w.y == 1.0f && w.z == 1.0f)
        return a;
    uint64_t handle = allocate_closure_node(sg, used, capacity, closure_mul_words);
    if (handle) {
        uint32_t node   = uint32_t(handle - 1);
        words[node]     = ClosureMulId;
        set_weight(words, node, w);
        words[node + 4] = uint32_t(a);
    }
    return handle;
}


OSL_SHADEOP uint64_t
osl_spirv_mul_closure_float(ShaderGlobals* sg, uint32_t* used,
                            uint32_t* words, int capacity, uint64_t a, float w)
{
    Color3 c(w);
    return osl_spirv_mul_closure_color(sg, used, words, capacity, a, &c);
}


OSL_SHADEOP uint64_t
osl_spirv_add_closure_closure(ShaderGlobals* sg, uint32_t* used,
                              uint32_t* words, int capacity, uint64_t a,
                              uint64_t b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    uint64_t handle = allocate_closure_node(sg, used, capacity, closure_add_words);
    if (handle) {
        uint32_t node   = uint32_t(handle - 1);
        words[node]     = ClosureAddId;
        words[node + 1] = uint32_t(a);
        words[node + 2] = uint32_t(b);
    }
    return handle;
}



// The shading point's index, for code generated for SPIR-V.
OSL_SHADEOP int
osl_spirv_shade_index(ShaderGlobals* sg)
{
    return get_shade_index(sg);
}



OSL_SHADEOP int
osl_range_check_err(int indexvalue, int length, ustringhash_pod symname,
                    void* sg, ustringhash_pod sourcefile, int sourceline,
                    ustringhash_pod groupname, int layer,
                    ustringhash_pod layername, ustringhash_pod shadername)
{
    if (indexvalue < 0 || indexvalue >= length) {
        OSL::errorfmt(
            sg,
            "Index [{}] out of range {}[0..{}]: {}:{} (group {}, layer {} {}, shader {})",
            indexvalue, ustringhash_from(symname), length - 1,
            ustringhash_from(sourcefile), sourceline,
            ustringhash_from(groupname), layer, ustringhash_from(layername),
            ustringhash_from(shadername));
        indexvalue = indexvalue >= length ? length - 1 : 0;
    }
    return indexvalue;
}
