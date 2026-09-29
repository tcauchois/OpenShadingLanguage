// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

// The renderer side of osltoy-vk: it owns the ShadingSystem, compiles a
// shader group to SPIR-V, generates the Slang fragment shader that calls it
// (and implements its renderer callbacks), and links the two.

#include <cstdint>
#include <string>
#include <vector>

#include <OpenImageIO/thread.h>

#include <OSL/oslexec.h>
#include <OSL/rendererservices.h>


OSL_NAMESPACE_BEGIN

class ToyRenderer final : public RendererServices {
public:
    ToyRenderer();
    ~ToyRenderer() override;

    ShadingSystem* shadingsys() const { return m_shadingsys; }

    int supports(string_view feature) const override
    {
        return feature == "SPIRV";
    }

    void set_shadergroup(ShaderGroupRef g)
    {
        OIIO::spin_lock lock(m_mutex);
        m_group = g;
    }
    ShaderGroupRef shadergroup()
    {
        OIIO::spin_lock lock(m_mutex);
        return m_group;
    }

    /// The output to show for shaders that don't set Ci.
    void set_display_output(ustring name) { m_display_output = name; }

    /// Timing (seconds) and size statistics of the last build.
    struct BuildStats {
        double osl_to_spirv   = 0;  // optimize + LLVM + SPIR-V backend
        double slang_compile  = 0;  // vertex and fragment shaders
        double link_validate  = 0;  // make_exports, link, validate
        double opt_time       = 0;  // ShadingSystem's runtime optimizer
        double llvm_time      = 0;  // ShadingSystem's LLVM total
        size_t osl_words      = 0;  // SPIR-V module sizes, in words
        size_t slang_words    = 0;
        size_t linked_words   = 0;
        size_t code_words     = 0;  // ... of the linked fragment module
        size_t constant_words = 0;
        size_t other_words    = 0;
        size_t eval_osl_instructions = 0;
        size_t function_var_bytes    = 0;  // Function-storage variables
        size_t private_var_bytes     = 0;  // Private-storage variables
        size_t groupdata_bytes       = 0;
        size_t closure_pool_bytes    = 0;
    };

    /// The compiled shaders for the current group: SPIR-V for the vertex and
    /// fragment stages, and the generated fragment source (for display).
    struct Program {
        std::vector<uint32_t> vertex, fragment;
        std::string fragment_source;
        size_t params_bytes = 0;  // interactive parameter block size
        bool uses_time      = false;
        BuildStats stats;
    };

    /// Compile the current group. Returns false and sets `err` on failure.
    bool build_program(Program& program, std::string& err);

    /// The group's current interactive parameter values (which ReParameter
    /// changes), padded for the shader's 16-word reads.
    std::vector<uint32_t> interactive_params();

    /// Decode the message buffer that the fragment shader filled (see the
    /// Renderer.message callback) into text, at most `max_messages`.
    std::string decode_messages(const uint32_t* words, size_t nwords,
                                size_t max_messages = 50);

    static constexpr size_t message_buffer_words = 1 << 16;

private:
    std::string fragment_template(const std::string& abi) const;

    ShadingSystem* m_shadingsys = nullptr;
    ShaderGroupRef m_group;
    ustring m_display_output;
    OIIO::spin_mutex m_mutex;
};

OSL_NAMESPACE_END
