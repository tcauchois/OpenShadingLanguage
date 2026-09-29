// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

// The 3D viewport: a QVulkanWindow that draws a sphere with the current
// shader group's fragment shader.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <QVulkanWindow>


namespace osltoyvk {

/// What the viewport draws with: the linked SPIR-V for each stage, and the
/// interactive parameter block.
struct ShaderProgram {
    std::vector<uint32_t> vertex, fragment;
    std::vector<uint32_t> params;
    bool animate = false;  // the shader uses time
};

/// Viewport-side statistics.
struct ViewStats {
    double pipeline_seconds = 0;  // shader module + pipeline creation
    double frame_ms         = 0;  // frame-to-frame time, averaged, over
    double fps              = 0;  // ... consecutive frames (or 0 if idle)
    double gpu_ms           = -1;  // from timestamps, if supported (or -1)
    std::string device;
    uint64_t frames      = 0;  // frames drawn
    int resource_inits   = 0;  // times the device resources were created
    bool pipeline_ready  = false;
};

class VkView;

class VkViewRenderer final : public QVulkanWindowRenderer {
public:
    explicit VkViewRenderer(VkView* view);

    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;

private:
    friend class VkView;
    VkView* m_view;
    QVulkanDeviceFunctions* m_df = nullptr;

    struct Buffer {
        VkBuffer buffer       = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped          = nullptr;
        size_t size           = 0;
    };
    Buffer make_buffer(size_t size, VkBufferUsageFlags usage);
    void destroy_buffer(Buffer& b);
    void build_pipeline();
    void destroy_pipeline();
    void write_descriptors(int frame);

    Buffer m_vertices, m_indices;
    uint32_t m_index_count = 0;
    // Per frame in flight: uniforms, message buffer, params
    static constexpr int max_frames = QVulkanWindow::MAX_CONCURRENT_FRAME_COUNT;
    Buffer m_uniforms[max_frames], m_messages[max_frames], m_params[max_frames];
    VkDescriptorPool m_dpool            = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dset_layout = VK_NULL_HANDLE;
    VkDescriptorSet m_dsets[max_frames] = {};
    VkPipelineLayout m_pipeline_layout  = VK_NULL_HANDLE;
    VkPipeline m_pipeline               = VK_NULL_HANDLE;
    VkQueryPool m_timestamps            = VK_NULL_HANDLE;
    float m_timestamp_period            = 0;
    bool m_message_pending[max_frames]  = {};
};

class VkView final : public QVulkanWindow {
    Q_OBJECT
public:
    VkView();

    QVulkanWindowRenderer* createRenderer() override;

    /// Use a new program (thread-safe; takes effect at the next frame).
    void set_program(const ShaderProgram& program);
    /// Update the interactive parameters.
    void set_params(const std::vector<uint32_t>& params);
    /// Show the shader's display output instead of lighting its closures.
    void set_display_output(bool on) { m_display_output = on; }
    void set_time(float t) { m_time = t; }
    void set_paused(bool p) { m_paused = p; }
    /// Render continuously (e.g., to measure the frame rate), even if
    /// nothing changes.
    void set_continuous(bool c)
    {
        m_continuous = c;
        requestUpdate();
    }
    void request_messages() { m_want_messages = true; }

    ViewStats stats();

    /// The current image.
    QImage snapshot();

    /// Called (on the GUI thread) with the decoded-later raw message
    /// buffer, after a frame for which messages were requested.
    std::function<void(std::vector<uint32_t>)> on_messages;
    /// Called (on the GUI thread) when building a pipeline fails.
    std::function<void(std::string)> on_error;

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    friend class VkViewRenderer;
    std::mutex m_mutex;
    ShaderProgram m_program;
    bool m_program_changed = false;
    bool m_params_changed  = false;
    std::atomic<bool> m_display_output { false };
    std::atomic<bool> m_paused { false };
    std::atomic<bool> m_continuous { false };
    bool m_last_frame_continuous = false;  // (for timing)
    std::atomic<bool> m_want_messages { false };
    std::atomic<float> m_time { 0 };
    ViewStats m_stats;
    // Orbit camera
    float m_yaw = 0.6f, m_pitch = 0.3f, m_distance = 3.2f;
    QPoint m_last_mouse;
};

}  // namespace osltoyvk
