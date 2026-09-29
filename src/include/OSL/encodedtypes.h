// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

#include <OSL/oslconfig.h>

OSL_NAMESPACE_BEGIN


// Substitute for variable argument list, which is traditionally used with a printf, with arrays
// of EncodedTypes to identify types contained in a blind payload of values.
enum class EncodedType : uint8_t {
    // OSL Shaders could encode these types
    kUstringHash = 0,
    kInt32,
    kFloat,

    // OSL library functions or renderer services encode additional native types
    kInt64,
    kDouble,
    kUInt32,
    kUInt64,
    kPointer,
    kTypeDesc,
    kCount
};

// Decode will use each EncodedType in the array to interpret the contents of the arg_values
// parameter alongwith a fmtlib specifier identified by the ustring that the format_hash represents.
// Contents of decoded_str are written over (not appended).
// Returns # of bytes read from arg_values
int
decode_message(uint64_t format_hash, int32_t arg_count,
               const EncodedType* arg_types, const uint8_t* arg_values,
               std::string& decoded_str);

namespace pvt {

constexpr inline uint32_t
size_of_encoded_type(EncodedType et)
{
    constexpr uint32_t SizeByEncodedType[] = {
        sizeof(OSL::ustringhash), sizeof(int32_t), sizeof(float),
        sizeof(int64_t),          sizeof(double),  sizeof(uint32_t),
        sizeof(uint64_t),         sizeof(void*),   sizeof(OSL::TypeDesc),
    };
    static_assert(sizeof(SizeByEncodedType) / sizeof(SizeByEncodedType[0])
                      == size_t(EncodedType::kCount),
                  "Keep array contents lined up with enum");
    return SizeByEncodedType[static_cast<int>(et)];
}

template<typename T>
struct TypeEncoder;  //Undefined on purpose; all types must have a specialization

template<> struct TypeEncoder<OSL::ustringhash> {
    using DataType                     = ustringhash_pod;
    static constexpr EncodedType value = EncodedType::kUstringHash;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const OSL::ustringhash& val) { return val.hash(); }
};

template<> struct TypeEncoder<OSL::ustring> {
    using DataType                     = ustringhash_pod;
    static constexpr EncodedType value = EncodedType::kUstringHash;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const OSL::ustring& val) { return val.hash(); }
};

template<> struct TypeEncoder<const char*> {
    using DataType                     = ustringhash_pod;
    static constexpr EncodedType value = EncodedType::kUstringHash;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const char* val) { return ustring(val).hash(); }
};

template<> struct TypeEncoder<std::string> {
    using DataType                     = ustringhash_pod;
    static constexpr EncodedType value = EncodedType::kUstringHash;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const std::string& val)
    {
        return ustring(val).hash();
    }
};

template<> struct TypeEncoder<int32_t> {
    using DataType                     = int32_t;
    static constexpr EncodedType value = EncodedType::kInt32;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const int32_t val) { return val; }
};

template<> struct TypeEncoder<float> {
    using DataType                     = float;
    static constexpr EncodedType value = EncodedType::kFloat;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const float val) { return val; }
};


template<> struct TypeEncoder<int64_t> {
    using DataType                     = int64_t;
    static constexpr EncodedType value = EncodedType::kInt64;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const int64_t val) { return val; }
};

// On macOS 10.+ ptrdiff_t is a long, but on linux this
// specialization would conflict with int64_t
#if defined(__APPLE__) && defined(__MACH__)
template<> struct TypeEncoder<ptrdiff_t> {
    using DataType                     = int64_t;
    static constexpr EncodedType value = EncodedType::kInt64;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const ptrdiff_t val) { return val; }
};
#endif

template<> struct TypeEncoder<double> {
    using DataType                     = double;
    static constexpr EncodedType value = EncodedType::kDouble;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const double val) { return val; }
};

template<> struct TypeEncoder<uint64_t> {
    using DataType                     = uint64_t;
    static constexpr EncodedType value = EncodedType::kUInt64;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const uint64_t val) { return val; }
};

template<> struct TypeEncoder<uint32_t> {
    using DataType                     = uint32_t;
    static constexpr EncodedType value = EncodedType::kUInt32;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const uint32_t val) { return val; }
};

template<typename T> struct TypeEncoder<T*> {
    // fmtlib only supports const void *, all other pointers must be
    // converted with a cast or helper like fmt::ptr(p)
    static_assert(
        (std::is_same<void*, T*>::value || std::is_same<const void*, T*>::value),
        "formatting of non-void pointers is disallowed, wrap with fmt::ptr");

    using DataType                     = const T*;
    static constexpr EncodedType value = EncodedType::kPointer;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const T* val) { return val; }
};

template<> struct TypeEncoder<OSL::TypeDesc> {
    // To avoid warnings about non-pod types when we PackArgs
    // we will encode into builtin type
    using DataType = uint64_t;
    static_assert(sizeof(OSL::TypeDesc) == sizeof(DataType), "unexpected");
    static constexpr EncodedType value = EncodedType::kTypeDesc;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const OSL::TypeDesc& val)
    {
        return OSL::bitcast<DataType>(val);
    }
};

// Promote thinner types
template<> struct TypeEncoder<int16_t> {
    using DataType                     = int32_t;
    static constexpr EncodedType value = EncodedType::kInt32;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const int16_t val) { return DataType(val); }
};

template<> struct TypeEncoder<int8_t> {
    using DataType                     = int32_t;
    static constexpr EncodedType value = EncodedType::kInt32;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const int8_t val) { return DataType(val); }
};

template<> struct TypeEncoder<uint16_t> {
    using DataType                     = uint32_t;
    static constexpr EncodedType value = EncodedType::kUInt32;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const uint16_t val) { return DataType(val); }
};

template<> struct TypeEncoder<uint8_t> {
    using DataType                     = uint32_t;
    static constexpr EncodedType value = EncodedType::kUInt32;
    static_assert(size_of_encoded_type(value) == sizeof(DataType),
                  "unexpected");
    static DataType Encode(const uint8_t val) { return DataType(val); }
};
/// Hash of a format specification for the *fmt functions. Normally this
/// makes a ustring (so the host can find the text again from the hash).
/// Shadeops compiled for SPIR-V can't create ustrings, so there it just
/// hashes the text; the shading system registers those string literals
/// with the host ustring table instead.
template<typename SpecifierT>
inline OSL::ustringhash
fmt_specification_hash(const SpecifierT& fmt_specification)
{
    return OSL::ustringhash { fmt_specification };
}

#ifdef OSL_SPIRV_SHADEOPS
// Marks a string literal that the host must know (to decode messages that
// refer to it by hash). The shading system interns the argument of each
// call when it loads the shadeops, then removes the calls.
extern "C" void
osl_spirv_intern_string(const char* s);

// String literals: hash them at compile time.
template<size_t N>
inline OSL::ustringhash
fmt_specification_hash(const char (&fmt_specification)[N])
{
    osl_spirv_intern_string(fmt_specification);
    // (The empty string's hash is 0, as for ustrings.)
    return OSL::ustringhash(
        N > 1 ? OIIO::Strutil::strhash(N - 1, fmt_specification) : 0);
}
#endif

}  // namespace pvt


OSL_NAMESPACE_END
