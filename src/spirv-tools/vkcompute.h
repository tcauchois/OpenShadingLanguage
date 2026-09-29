// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

// A minimal Vulkan compute runner for OSL shader groups compiled to SPIR-V,
// shared by testshade and osltoy. It compiles a Slang kernel that calls
// eval_osl, links it with the OSL-generated SPIR-V library, and dispatches
// it over a 2D grid, with storage buffers and textures.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <OSL/oslconfig.h>


OSL_NAMESPACE_BEGIN

namespace spirv {

class VulkanCompute {
public:
    VulkanCompute();
    ~VulkanCompute();
    VulkanCompute(const VulkanCompute&)            = delete;
    VulkanCompute& operator=(const VulkanCompute&) = delete;

    /// Create the Vulkan instance and device. Returns false and sets
    /// `err` if no suitable device is available.
    bool init(std::string& err);

    /// Name of the Vulkan device in use.
    const std::string& device_name() const;

    /// Compile Slang `source` (a compute shader with entry point `main`)
    /// to SPIR-V. `imports` lists functions it declares `[__extern]`,
    /// which are turned into imports so that the result can be linked.
    /// `include_sources` are (name, text) pairs available to `#include`.
    static bool
    compile_slang(const std::string& source,
                  const std::vector<std::pair<std::string, std::string>>&
                      include_sources,
                  const std::vector<std::string>& imports,
                  std::vector<uint32_t>& spirv, std::string& err);

    /// Create the compute pipeline from a fully linked SPIR-V module. The
    /// shader may use up to `max_buffers` storage buffers, at set 0,
    /// bindings 0, 1, ..., and at most 128 bytes of push constants.
    bool set_pipeline(const std::vector<uint32_t>& spirv, std::string& err);

    static constexpr int max_buffers = 4;

    /// Textures, bound at set 0 after the storage buffers:
    ///
    ///   binding max_buffers:     Texture2D textures[max_textures];
    ///   binding max_buffers + 1: SamplerState samplers[num_samplers];
    ///
    /// Unused texture slots hold a 1x1 black texture. Sampler
    /// `sampler_index(swrap, twrap, nearest)` has those wrap modes (see
    /// `SamplerWrap`), trilinear (or, with `nearest`, nearest-neighbor)
    /// filtering, and anisotropic filtering where the device supports it.
    static constexpr int max_textures = 64;
    enum SamplerWrap { WrapPeriodic = 0, WrapBlack, WrapClamp, WrapMirror };
    static constexpr int num_samplers = 32;
    static constexpr int sampler_index(int swrap, int twrap, bool nearest)
    {
        return (swrap * 4 + twrap) * 2 + (nearest ? 1 : 0);
    }

    /// Add a texture from its MIP levels (each RGBA float, rows top to
    /// bottom, each level half the size of the one before). Returns its
    /// index, or -1 and sets `err` on failure.
    int add_texture(int width, int height,
                    const std::vector<std::vector<float>>& levels,
                    std::string& err);

    /// Remove all textures.
    void clear_textures();

    /// Dispatch a (xgroups, ygroups) grid of workgroups with the given
    /// push constants. Storage buffer i is initialized from buffers[i]
    /// (whose size is the buffer's size) and its contents are copied back
    /// there afterwards.
    bool dispatch(uint32_t xgroups, uint32_t ygroups, const void* push_data,
                  uint32_t push_size, std::vector<std::vector<uint8_t>>& buffers,
                  std::string& err);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace spirv

OSL_NAMESPACE_END
