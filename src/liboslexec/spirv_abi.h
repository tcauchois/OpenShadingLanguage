// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

// The ABI between the SPIR-V shadeops (rs_spirv.cpp, which runs in the
// shader group) and the code that generates the group and its Slang
// declarations (llvm_spirv.cpp, spirv_slang_abi.cpp, which run in the
// shading system): sizes, codes, and the layouts of the structs passed to
// the renderer's shader. Each is defined once, here.
//
// This is included by rs_spirv.cpp, which is compiled to LLVM bitcode, so
// it must stay plain C++ with no dependencies beyond oslconfig.h.

#include <cstdint>

#include <OSL/oslconfig.h>


// Sizes that appear in the Slang declarations of the structs below, so
// they're macros (to be stringified) as well as constants.
#define OSL_SPIRV_MATRIX_FLOATS 16
#define OSL_SPIRV_ATTRIBUTE_WORDS 16
#define OSL_SPIRV_MAX_MESSAGE_ARGS 64
#define OSL_SPIRV_MAX_MESSAGE_WORDS 128
#define OSL_SPIRV_TEXTURE_POINTS 4
#define OSL_SPIRV_TEXTURE_ST_FLOATS 8     // (s, t) for each point
#define OSL_SPIRV_TEXTURE_RGBA_FLOATS 16  // RGBA for each point

#define OSL_SPIRV_STRINGIZE_(x) #x
#define OSL_SPIRV_STRINGIZE(x) OSL_SPIRV_STRINGIZE_(x)


OSL_NAMESPACE_BEGIN
namespace pvt {
namespace spirv_abi {

// The structs that the renderer's callbacks fill or read. They're all made
// of 32-bit words. (In Slang they are OSLMatrix, OSLAttribute,
// OSLMessageArgs and OSLTextureSample; see spirv_callbacks.h.)

struct SpirvMatrix {
    float m[OSL_SPIRV_MATRIX_FLOATS];
};

struct SpirvAttribute {
    // Enough for a matrix, or a float3 with derivatives
    uint32_t words[OSL_SPIRV_ATTRIBUTE_WORDS];
};

constexpr int max_message_args  = OSL_SPIRV_MAX_MESSAGE_ARGS;
constexpr int max_message_words = OSL_SPIRV_MAX_MESSAGE_WORDS;

struct SpirvMessageArgs {
    uint32_t types[max_message_args];   // EncodedType of each arg
    uint32_t words[max_message_words];  // packed values, as 32-bit words
};

// Sampling a texture at a few points that share a filter footprint. The
// renderer's shader finds the texture, and does only the sampling: OSL does
// the rest (environment mapping, filter width and blur, derivatives, which
// channels).
struct SpirvTextureSample {
    float st[OSL_SPIRV_TEXTURE_ST_FLOATS];  // in: s and t of each of npoints
    float dx[2];  // in: the filter footprint: d(s,t)/dx and d(s,t)/dy
    float dy[2];
    float rgba[OSL_SPIRV_TEXTURE_RGBA_FLOATS];  // out: each point's color
    uint32_t info[4];  // out: the texture's channels, width and height
};

static_assert(sizeof(SpirvMatrix) == 4 * OSL_SPIRV_MATRIX_FLOATS, "ABI");
static_assert(sizeof(SpirvAttribute) == 4 * OSL_SPIRV_ATTRIBUTE_WORDS, "ABI");
static_assert(sizeof(SpirvMessageArgs)
                  == 4 * (max_message_args + max_message_words),
              "ABI");
static_assert(sizeof(SpirvTextureSample)
                  == 4
                         * (OSL_SPIRV_TEXTURE_ST_FLOATS + 4
                            + OSL_SPIRV_TEXTURE_RGBA_FLOATS + 4),
              "ABI");


// Codes passed to the callbacks. (The generated Slang ABI names them:
// OSL_MESSAGE_ERROR, OSL_MATRIX_FROM_HANDLE, etc.)

// Kinds of message.
enum SpirvMessageKind : uint32_t { Error = 0, Warning, Print, FilePrint };

// Which matrix to get: by transformation handle or by name.
enum SpirvMatrixSource : uint32_t { MatrixFromHandle = 0, MatrixFromName = 1 };


// Messages in slots: each is a header and the value's 32-bit words (as
// separate globals). The shading system creates the headers with this
// layout, and zeroed.

enum MessageState : uint32_t { NoMessage = 0, Queried, HasData };

struct MessageHeader {
    uint64_t type;  // TypeDesc bits
    uint64_t sourcefile;
    uint32_t state;
    uint32_t layer;
    uint32_t line;
};


// Nodes in a closure pool (a group's groupdata has a pool of words). A
// closure value is a handle: 1 + the index of its node's first word, or 0
// for no closure. The nodes are
//
//   component: id, weight (3 floats), the closure's params (host layout)
//   mul:       ClosureMulId, weight (3 floats), handle
//   add:       ClosureAddId, handle, handle
//
// with the same semantics as opclosure.cpp's.

constexpr uint32_t ClosureMulId = uint32_t(-1);  // ClosureColor::MUL
constexpr uint32_t ClosureAddId = uint32_t(-2);  // ClosureColor::ADD

// Words before a component's params (its id and weight), and the whole
// mul and add nodes.
constexpr int closure_component_header_words = 4;
constexpr int closure_mul_words              = 5;
constexpr int closure_add_words              = 3;

}  // namespace spirv_abi
}  // namespace pvt
OSL_NAMESPACE_END
