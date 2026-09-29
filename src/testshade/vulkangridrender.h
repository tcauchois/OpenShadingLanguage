// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

#include <memory>

#include <OSL/journal.h>
#include <OSL/oslexec.h>

#include "simplerend.h"


OSL_NAMESPACE_BEGIN

namespace spirv {
class VulkanCompute;
}


/// Shades the testshade grid on a Vulkan device, by compiling the shader
/// group to SPIR-V and linking it into a generated compute kernel.
class VulkanGridRenderer final : public SimpleRenderer {
public:
    /// How to set up the shader globals, matching testshade's options.
    struct GridParams {
        bool pixelcenters = false;
        bool vary_Pdxdy   = false;
        bool vary_udxdy   = false;
        bool vary_vdxdy   = false;
        float uscale = 1, vscale = 1;
        float uoffset = 0, voffset = 0;
        int raytype_bit = 0;
        Matrix44 shader2common, object2common;
        bool print_outputs = false;  // Print each point's outputs (--print)
        std::vector<ustring> output_names;  // as testshade prints them
    };

    VulkanGridRenderer();
    ~VulkanGridRenderer() override;

    int supports(string_view feature) const override
    {
        if (feature == "SPIRV")
            return true;
        return SimpleRenderer::supports(feature);
    }

    void set_grid_params(const GridParams& params) { m_grid = params; }

    void prepare_render() final;
    void render(int xres, int yres) final;
    void finalize_pixel_buffer() final;
    void clear() final;

    /// Record the messages (printf, errors, ...) from the renders so far
    /// into the journal, in render then shading-point order.
    void journal_messages(void* journal_buffer);

private:
    bool build_pipeline();
    std::string kernel_source(const std::vector<std::string>& fields,
                              const std::vector<TypeDesc>& types) const;
    std::vector<uint8_t> matrix_table() const;
    void load_textures(ShaderGroup* group);
    void journal_messages(OSL::journal::Writer& jw,
                          const std::vector<uint8_t>& messages);

    std::unique_ptr<spirv::VulkanCompute> m_vk;
    GridParams m_grid;
    bool m_ready = false;
    std::vector<TypeDesc> m_output_types;  // per output, in outputbuf order
    std::vector<size_t> m_output_offsets;  // word offset within a pixel
    size_t m_pixel_words = 0;
    std::vector<std::vector<uint8_t>> m_buffers;
    std::vector<std::vector<uint8_t>> m_messages;  // per render
    std::string m_texture_table;  // kernel code finding a texture by name
    bool m_closures          = false;  // the group has closures
    int m_closure_pool_words = 0;      // and their pool's size
    std::string closure_printer_source(int pool_words) const;
    std::string format_closure(const uint32_t* words, size_t n,
                               uint32_t root) const;
};


OSL_NAMESPACE_END
