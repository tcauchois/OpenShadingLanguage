// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

// The SPIR-V host-side codegen's shared declarations: the renderer
// callbacks table (used to generate both the LLVM declarations, in
// llvm_spirv.cpp, and the Slang ABI, in spirv_slang_abi.cpp), and the
// target description.

#include <vector>

#include <llvm/ADT/StringRef.h>

#include <OSL/oslconfig.h>


OSL_NAMESPACE_BEGIN
namespace pvt {

/// Name of the function exported by SPIR-V shader groups.
extern const char* spirv_entry_name;

/// Target for SPIR-V compilation: logical addressing SPIR-V 1.6 for Vulkan
/// 1.3, matching what clang uses for this triple.
extern const char* spirv_triple;
extern const char* spirv_data_layout;

// The functions that the renderer's shader supplies to SPIR-V shader
// groups, with their Slang declarations. Each has a final out (or in)
// parameter that is a struct holding an array of 32-bit words. These must
// match rs_spirv.cpp. (Integers are unsigned because LLVM's SPIR-V is,
// and the types must match exactly across the link: e.g., an attribute
// array index of -1, meaning "not an array lookup", is 0xffffffff.)
struct SpirvCallback {
    const char* name;
    const char* struct_name;  // Slang/LLVM name of the last param's struct
    const char* fields;       // Slang fields of that struct
    const char* ret;          // Slang return type
    const char* method;       // IOSLRenderer method name
    const char* params;       // Slang parameter list
    const char* args;         // Parameter names, to forward the call
    const char* keep_call;    // Slang expression calling it with struct `s`
};

/// The renderer callbacks, in ABI order.
const std::vector<SpirvCallback>&
spirv_callbacks();

/// Is `name` one of the callbacks?
bool
is_spirv_callback(llvm::StringRef name);

}  // namespace pvt
OSL_NAMESPACE_END
