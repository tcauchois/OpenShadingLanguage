// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

// Helpers for linking OSL-generated SPIR-V libraries with renderer-authored
// shaders. OSL compiles a shader group to a SPIR-V module that exports an
// entry function (e.g. "eval_osl") and imports renderer callbacks. Front ends
// such as Slang can declare such functions `[__extern]` but emit them only as
// empty stubs without an Import linkage decoration, so the modules can't be
// linked directly. These helpers patch the stubs and link the modules.

#include <cstdint>
#include <string>
#include <vector>

#include <OSL/oslconfig.h>


OSL_NAMESPACE_BEGIN

namespace spirv {

/// Turn each bodiless function named in `names` into an imported
/// declaration: adds `OpCapability Linkage` if needed and decorates the
/// function with `LinkageAttributes "<name>" Import`. Functions are matched
/// by their OpName. Returns false and sets `err` if a name isn't found, is
/// not a function, or already has a body.
bool
make_imports(std::vector<uint32_t>& binary,
             const std::vector<std::string>& names, std::string& err);

/// Make each named function (which must have a body) available to other
/// modules: decorates it with `LinkageAttributes "<name>" Export`. Front
/// ends like Slang can't express exports from a shader, but will keep
/// functions that the shader calls, e.g. renderer callbacks for OSL.
bool
make_exports(std::vector<uint32_t>& binary,
             const std::vector<std::string>& names, std::string& err);

/// Work around LLVM SPIR-V backend bugs in `binary`: moves any instructions
/// that were emitted between an OpSelectionMerge or OpLoopMerge and the
/// branch that must immediately follow it to before the merge.
bool
fix_structured_merges(std::vector<uint32_t>& binary, std::string& err);

/// Link `modules` into one module, resolving imports against exports.
/// The result has no remaining Linkage capability or decorations if every
/// import was resolved. Returns false and sets `err` on failure.
bool
link(const std::vector<std::vector<uint32_t>>& modules,
     std::vector<uint32_t>& linked, std::string& err);

/// Validate `binary` for the Vulkan 1.3 environment. If `allow_linkage` is
/// true, the validation is done against the universal SPIR-V 1.6 rules so
/// that unlinked libraries (which use the Linkage capability) are accepted.
bool
validate(const std::vector<uint32_t>& binary, bool allow_linkage,
         std::string& err);

/// Does `binary` declare the Linkage capability (i.e., is it a library or
/// a module with unresolved imports)?
bool
has_linkage(const std::vector<uint32_t>& binary);

/// Size statistics for a SPIR-V module.
struct ModuleStats {
    size_t total_words    = 0;
    size_t code_words     = 0;  ///< in function bodies
    size_t constant_words = 0;  ///< OpConstant* and OpSpecConstant*
    size_t other_words    = 0;  ///< types, decorations, names, ...
    size_t function_variable_bytes = 0;  ///< Function-storage OpVariables
    size_t private_variable_bytes  = 0;  ///< Private-storage OpVariables
    /// Instructions in each function, by name (or "%<id>" if unnamed).
    std::vector<std::pair<std::string, size_t>> function_instructions;
};

/// Compute size statistics for `binary`. (Variable sizes assume 4-byte
/// scalars, except 64-bit and 8/16-bit ones, and no padding.)
bool
module_stats(const std::vector<uint32_t>& binary, ModuleStats& stats,
             std::string& err);

/// Convenience file I/O for SPIR-V binaries.
bool
read_file(const std::string& filename, std::vector<uint32_t>& binary,
          std::string& err);
bool
write_file(const std::string& filename, const std::vector<uint32_t>& binary,
           std::string& err);

}  // namespace spirv

OSL_NAMESPACE_END
