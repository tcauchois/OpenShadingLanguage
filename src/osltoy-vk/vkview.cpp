// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include "vkview.h"

#include <chrono>
#include <cmath>
#include <cstring>

#include <QMetaObject>
#include <QMouseEvent>
#include <QVulkanFunctions>
#include <QWheelEvent>

#include "sphere.h"


namespace osltoyvk {

namespace {

// Must match the Uniforms struct in the shaders (see toyrenderer.cpp).
struct Uniforms {
    float view_proj[16];  // row-major (Slang's default layout, for mul(M, v))
    float camera_pos[4];  // xyz, time
    float light_dir[4];   // xyz (toward the light), intensity
    float light_color[4];  // rgb, display output (0 = Ci, 1 = output)
    float ambient[4];      // rgb, exposure
    float viewport[4];     // width, height, frame, unused
};

using Clock = std::chrono::steady_clock;

double
seconds_since(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// 4x4 matrices, row-major in memory: m[row * 4 + col].
struct Mat4 {
    float m[16] = {};
    static Mat4 identity()
    {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
        return r;
    }
    Mat4 operator*(const Mat4& b) const
    {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    r.m[i * 4 + j] += m[i * 4 + k] * b.m[k * 4 + j];
        return r;
    }
};

// A view matrix looking from `eye` at the origin, with y up.
Mat4
look_at_origin(const float eye[3])
{
    float f[3] = { -eye[0], -eye[1], -eye[2] };
    float fl   = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& c : f)
        c /= fl;
    float up[3] = { 0, 1, 0 };
    float s[3]  = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                    f[0] * up[1] - f[1] * up[0] };
    float sl    = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (float& c : s)
        c /= sl;
    float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                   s[0] * f[1] - s[1] * f[0] };
    Mat4 r     = Mat4::identity();
    for (int c = 0; c < 3; ++c) {
        r.m[0 * 4 + c] = s[c];
        r.m[1 * 4 + c] = u[c];
        r.m[2 * 4 + c] = -f[c];
    }
    r.m[0 * 4 + 3] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    r.m[1 * 4 + 3] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    r.m[2 * 4 + 3] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    return r;
}

// Vulkan perspective projection (depth 0..1, y down in clip space).
Mat4
perspective(float fovy, float aspect, float znear, float zfar)
{
    float t = 1.0f / std::tan(fovy / 2);
    Mat4 r;
    r.m[0 * 4 + 0] = t / aspect;
    r.m[1 * 4 + 1] = -t;
    r.m[2 * 4 + 2] = zfar / (znear - zfar);
    r.m[2 * 4 + 3] = znear * zfar / (znear - zfar);
    r.m[3 * 4 + 2] = -1;
    return r;
}

}  // namespace



VkView::VkView()
{
    // The fragment shader encodes sRGB itself, so prefer a UNORM swapchain.
    setPreferredColorFormats({ VK_FORMAT_B8G8R8A8_UNORM,
                               VK_FORMAT_R8G8B8A8_UNORM });
    // The OSL shader code uses 64-bit (string hashes, closure handles),
    // 16-bit, and 8-bit integers.
    setEnabledFeaturesModifier([](VkPhysicalDeviceFeatures2& f) {
        f.features.shaderInt64 = VK_TRUE;
        f.features.shaderInt16 = VK_TRUE;
        static VkPhysicalDeviceVulkan12Features f12 {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
        };
        f12.shaderInt8                        = VK_TRUE;
        f12.storageBuffer8BitAccess           = VK_TRUE;
        f12.uniformAndStorageBuffer8BitAccess = VK_TRUE;
        f12.pNext                             = f.pNext;
        f.pNext                               = &f12;
    });
}



QVulkanWindowRenderer*
VkView::createRenderer()
{
    return new VkViewRenderer(this);
}



void
VkView::set_program(const ShaderProgram& program)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_program         = program;
    m_program_changed = true;
    m_params_changed  = true;
    requestUpdate();
}



void
VkView::set_params(const std::vector<uint32_t>& params)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_program.params = params;
    m_params_changed = true;
    requestUpdate();
}



QImage
VkView::snapshot()
{
    return grab();  // (which handles the swapchain's channel order)
}



ViewStats
VkView::stats()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}



void
VkView::mousePressEvent(QMouseEvent* e)
{
    m_last_mouse = e->position().toPoint();
}



void
VkView::mouseMoveEvent(QMouseEvent* e)
{
    if (!(e->buttons() & Qt::LeftButton))
        return;
    QPoint p = e->position().toPoint();
    QPoint d = p - m_last_mouse;
    m_last_mouse = p;
    m_yaw -= d.x() * 0.01f;
    m_pitch = std::clamp(m_pitch + d.y() * 0.01f, -1.5f, 1.5f);
    requestUpdate();
}



void
VkView::wheelEvent(QWheelEvent* e)
{
    m_distance *= std::pow(0.999f, float(e->angleDelta().y()));
    m_distance = std::clamp(m_distance, 1.3f, 20.0f);
    requestUpdate();
}



VkViewRenderer::VkViewRenderer(VkView* view) : m_view(view) {}



VkViewRenderer::Buffer
VkViewRenderer::make_buffer(size_t size, VkBufferUsageFlags usage)
{
    VkDevice dev = m_view->device();
    Buffer b;
    b.size = std::max(size, size_t(16));
    VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = b.size;
    bci.usage = usage;
    m_df->vkCreateBuffer(dev, &bci, nullptr, &b.buffer);
    VkMemoryRequirements req;
    m_df->vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = m_view->hostVisibleMemoryIndex();
    m_df->vkAllocateMemory(dev, &mai, nullptr, &b.memory);
    m_df->vkBindBufferMemory(dev, b.buffer, b.memory, 0);
    m_df->vkMapMemory(dev, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    return b;
}



void
VkViewRenderer::destroy_buffer(Buffer& b)
{
    VkDevice dev = m_view->device();
    if (b.memory) {
        m_df->vkUnmapMemory(dev, b.memory);
        m_df->vkFreeMemory(dev, b.memory, nullptr);
    }
    if (b.buffer)
        m_df->vkDestroyBuffer(dev, b.buffer, nullptr);
    b = Buffer();
}



void
VkViewRenderer::initResources()
{
    VkDevice dev = m_view->device();
    m_df         = m_view->vulkanInstance()->deviceFunctions(dev);

    {
        std::lock_guard<std::mutex> lock(m_view->m_mutex);
        m_view->m_stats.device = m_view->physicalDeviceProperties()->deviceName;
        const VkPhysicalDeviceLimits& lim
            = m_view->physicalDeviceProperties()->limits;
        m_timestamp_period = lim.timestampComputeAndGraphics
                                 ? lim.timestampPeriod
                                 : 0.0f;
    }

    // The sphere
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    make_sphere(64, vertices, indices);
    m_vertices = make_buffer(vertices.size() * sizeof(Vertex),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(m_vertices.mapped, vertices.data(), vertices.size() * sizeof(Vertex));
    m_indices = make_buffer(indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    memcpy(m_indices.mapped, indices.data(), indices.size() * 4);
    m_index_count = uint32_t(indices.size());

    // Per-frame buffers. (The message buffer size must match
    // ToyRenderer::message_buffer_words.)
    for (int f = 0; f < max_frames; ++f) {
        m_uniforms[f] = make_buffer(sizeof(Uniforms),
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        m_messages[f] = make_buffer((1 << 16) * 4,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        m_params[f]   = make_buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }

    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    nullptr };
    bindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bindings[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo dslci {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    dslci.bindingCount = 3;
    dslci.pBindings    = bindings;
    m_df->vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &m_dset_layout);

    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, max_frames },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * max_frames },
    };
    VkDescriptorPoolCreateInfo dpci { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = max_frames;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = sizes;
    m_df->vkCreateDescriptorPool(dev, &dpci, nullptr, &m_dpool);
    for (int f = 0; f < max_frames; ++f) {
        VkDescriptorSetAllocateInfo dsai {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        dsai.descriptorPool     = m_dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_dset_layout;
        m_df->vkAllocateDescriptorSets(dev, &dsai, &m_dsets[f]);
        write_descriptors(f);
    }

    VkPipelineLayoutCreateInfo plci { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts    = &m_dset_layout;
    m_df->vkCreatePipelineLayout(dev, &plci, nullptr, &m_pipeline_layout);

    if (m_timestamp_period > 0) {
        VkQueryPoolCreateInfo qpci { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 2 * max_frames;
        m_df->vkCreateQueryPool(dev, &qpci, nullptr, &m_timestamps);
    }

    // QVulkanWindow releases all resources (and then calls this again)
    // when the window is hidden or minimized, so rebuild the pipeline and
    // the parameters for any program we already have.
    std::lock_guard<std::mutex> lock(m_view->m_mutex);
    ++m_view->m_stats.resource_inits;
    if (m_view->m_program.fragment.size()) {
        m_view->m_program_changed = true;
        m_view->m_params_changed  = true;
    }
}



void
VkViewRenderer::write_descriptors(int f)
{
    VkDescriptorBufferInfo infos[3] = {
        { m_uniforms[f].buffer, 0, VK_WHOLE_SIZE },
        { m_messages[f].buffer, 0, VK_WHOLE_SIZE },
        { m_params[f].buffer, 0, VK_WHOLE_SIZE },
    };
    VkWriteDescriptorSet writes[3] = {};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_dsets[f];
        writes[i].dstBinding      = uint32_t(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo     = &infos[i];
    }
    m_df->vkUpdateDescriptorSets(m_view->device(), 3, writes, 0, nullptr);
}



void
VkViewRenderer::build_pipeline()
{
    VkDevice dev = m_view->device();
    std::vector<uint32_t> vs, fs;
    {
        std::lock_guard<std::mutex> lock(m_view->m_mutex);
        vs = m_view->m_program.vertex;
        fs = m_view->m_program.fragment;
    }
    m_df->vkDeviceWaitIdle(dev);
    destroy_pipeline();
    if (vs.empty() || fs.empty())
        return;
    auto t0 = Clock::now();

    auto module = [&](const std::vector<uint32_t>& code) {
        VkShaderModuleCreateInfo smci {
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
        };
        smci.codeSize    = code.size() * 4;
        smci.pCode       = code.data();
        VkShaderModule m = VK_NULL_HANDLE;
        m_df->vkCreateShaderModule(dev, &smci, nullptr, &m);
        return m;
    };
    VkShaderModule vsm = module(vs), fsm = module(fs);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName  = "main";
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription vbind { 0, sizeof(Vertex),
                                            VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vattrs[4] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, P) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, N) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv) },
        { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, dPdu) },
    };
    VkPipelineVertexInputStateCreateInfo vin {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    vin.vertexBindingDescriptionCount   = 1;
    vin.pVertexBindingDescriptions      = &vbind;
    vin.vertexAttributeDescriptionCount = 4;
    vin.pVertexAttributeDescriptions    = vattrs;

    VkPipelineInputAssemblyStateCreateInfo ia {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo rs {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_BACK_BIT;
    // (The sphere's triangles are counter-clockwise as seen from outside;
    // the projection's y flip and Vulkan's y-down framebuffer cancel out.)
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    ms.rasterizationSamples = m_view->sampleCountFlagBits();
    VkPipelineDepthStencilStateCreateInfo ds {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState att {};
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT,
                             VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyn;

    VkGraphicsPipelineCreateInfo gpci {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
    };
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &vin;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState      = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState   = &ms;
    gpci.pDepthStencilState  = &ds;
    gpci.pColorBlendState    = &cb;
    gpci.pDynamicState       = &dy;
    gpci.layout              = m_pipeline_layout;
    gpci.renderPass          = m_view->defaultRenderPass();
    VkResult r = m_df->vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci,
                                                 nullptr, &m_pipeline);
    m_df->vkDestroyShaderModule(dev, vsm, nullptr);
    m_df->vkDestroyShaderModule(dev, fsm, nullptr);
    double seconds = seconds_since(t0);
    if (r != VK_SUCCESS) {
        m_pipeline = VK_NULL_HANDLE;
        if (m_view->on_error) {
            std::string msg = "vkCreateGraphicsPipelines failed (VkResult "
                              + std::to_string(int(r)) + ")";
            QMetaObject::invokeMethod(m_view, [v = m_view, msg] {
                v->on_error(msg);
            });
        }
    }
    std::lock_guard<std::mutex> lock(m_view->m_mutex);
    m_view->m_stats.pipeline_seconds = seconds;
}



void
VkViewRenderer::destroy_pipeline()
{
    if (m_pipeline)
        m_df->vkDestroyPipeline(m_view->device(), m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;
}



void
VkViewRenderer::initSwapChainResources()
{
}



void
VkViewRenderer::releaseSwapChainResources()
{
}



void
VkViewRenderer::releaseResources()
{
    VkDevice dev = m_view->device();
    m_df->vkDeviceWaitIdle(dev);
    destroy_pipeline();
    if (m_timestamps)
        m_df->vkDestroyQueryPool(dev, m_timestamps, nullptr);
    m_timestamps = VK_NULL_HANDLE;
    m_df->vkDestroyPipelineLayout(dev, m_pipeline_layout, nullptr);
    m_df->vkDestroyDescriptorPool(dev, m_dpool, nullptr);
    m_df->vkDestroyDescriptorSetLayout(dev, m_dset_layout, nullptr);
    m_pipeline_layout = VK_NULL_HANDLE;
    m_dpool           = VK_NULL_HANDLE;
    m_dset_layout     = VK_NULL_HANDLE;
    destroy_buffer(m_vertices);
    destroy_buffer(m_indices);
    for (int f = 0; f < max_frames; ++f) {
        destroy_buffer(m_uniforms[f]);
        destroy_buffer(m_messages[f]);
        destroy_buffer(m_params[f]);
    }
}



void
VkViewRenderer::startNextFrame()
{
    static Clock::time_point last_frame = Clock::now();
    const int f = m_view->currentFrame();
    VkDevice dev = m_view->device();

    // This frame's buffers are no longer in use by the GPU (QVulkanWindow
    // waits for the frame's fence), so read back its messages and timings.
    if (m_message_pending[f]) {
        m_message_pending[f] = false;
        const uint32_t* w    = (const uint32_t*)m_messages[f].mapped;
        std::vector<uint32_t> words(w, w + std::min<size_t>(w[0] + 2, 1 << 16));
        if (m_view->on_messages)
            QMetaObject::invokeMethod(m_view, [v = m_view, words] {
                v->on_messages(words);
            });
    }
    if (m_timestamps) {
        uint64_t ts[2] = {};
        if (m_df->vkGetQueryPoolResults(dev, m_timestamps, 2 * f, 2,
                                        sizeof(ts), ts, sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT)
            == VK_SUCCESS) {
            std::lock_guard<std::mutex> lock(m_view->m_mutex);
            m_view->m_stats.gpu_ms = (ts[1] - ts[0]) * m_timestamp_period * 1e-6;
        }
    }

    bool program_changed = false, params_changed = false;
    std::vector<uint32_t> params;
    bool animate;
    {
        std::lock_guard<std::mutex> lock(m_view->m_mutex);
        program_changed           = m_view->m_program_changed;
        params_changed            = m_view->m_params_changed;
        m_view->m_program_changed = false;
        m_view->m_params_changed  = false;
        if (params_changed)
            params = m_view->m_program.params;
        animate = m_view->m_program.animate;
    }
    if (program_changed)
        build_pipeline();
    if (params_changed) {
        // Grow the parameter buffers if needed (all frames, since each has
        // its own copy).
        m_df->vkDeviceWaitIdle(dev);
        for (int i = 0; i < max_frames; ++i) {
            if (params.size() * 4 > m_params[i].size) {
                destroy_buffer(m_params[i]);
                m_params[i] = make_buffer(params.size() * 4,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                write_descriptors(i);
            }
            memcpy(m_params[i].mapped, params.data(), params.size() * 4);
        }
    }

    // Uniforms
    QSize size   = m_view->swapChainImageSize();
    float cp     = std::cos(m_view->m_pitch), sp = std::sin(m_view->m_pitch);
    float eye[3] = { m_view->m_distance * cp * std::sin(m_view->m_yaw),
                     m_view->m_distance * sp,
                     m_view->m_distance * cp * std::cos(m_view->m_yaw) };
    Mat4 vpm     = perspective(0.7f, float(size.width()) / size.height(), 0.1f,
                               100.0f)
               * look_at_origin(eye);
    Uniforms u {};
    memcpy(u.view_proj, vpm.m, sizeof(u.view_proj));
    u.camera_pos[0] = eye[0];
    u.camera_pos[1] = eye[1];
    u.camera_pos[2] = eye[2];
    u.camera_pos[3] = m_view->m_time;
    float L[3]      = { -0.7f, 0.8f, 0.5f };  // from the upper left
    float Ll        = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    u.light_dir[0]  = L[0] / Ll;
    u.light_dir[1]  = L[1] / Ll;
    u.light_dir[2]  = L[2] / Ll;
    u.light_dir[3]  = 2.5f;  // intensity
    u.light_color[0] = u.light_color[1] = u.light_color[2] = 1.0f;
    u.light_color[3] = m_view->m_display_output ? 1.0f : 0.0f;
    u.ambient[0] = u.ambient[1] = u.ambient[2] = 0.25f;
    u.ambient[3] = 1.0f;  // exposure
    u.viewport[0] = float(size.width());
    u.viewport[1] = float(size.height());
    memcpy(m_uniforms[f].mapped, &u, sizeof(u));

    bool messages = m_view->m_want_messages.exchange(false);
    if (messages)
        memset(m_messages[f].mapped, 0, 8);  // count and overflow flag

    VkCommandBuffer cmd = m_view->currentCommandBuffer();
    if (m_timestamps)
        m_df->vkCmdResetQueryPool(cmd, m_timestamps, 2 * f, 2);
    VkClearValue clear[3] = {};
    clear[0].color        = { { 0.12f, 0.12f, 0.14f, 1.0f } };
    clear[1].depthStencil = { 1.0f, 0 };
    clear[2].color        = clear[0].color;
    VkRenderPassBeginInfo rpbi { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass               = m_view->defaultRenderPass();
    rpbi.framebuffer              = m_view->currentFramebuffer();
    rpbi.renderArea.extent.width  = uint32_t(size.width());
    rpbi.renderArea.extent.height = uint32_t(size.height());
    rpbi.clearValueCount = m_view->sampleCountFlagBits() > VK_SAMPLE_COUNT_1_BIT
                               ? 3
                               : 2;
    rpbi.pClearValues = clear;
    if (m_timestamps)
        m_df->vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  m_timestamps, 2 * f);
    m_df->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    if (m_pipeline) {
        VkViewport viewport { 0, 0, float(size.width()), float(size.height()),
                              0, 1 };
        VkRect2D scissor { { 0, 0 },
                           { uint32_t(size.width()), uint32_t(size.height()) } };
        m_df->vkCmdSetViewport(cmd, 0, 1, &viewport);
        m_df->vkCmdSetScissor(cmd, 0, 1, &scissor);
        m_df->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeline);
        m_df->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_pipeline_layout, 0, 1, &m_dsets[f], 0,
                                      nullptr);
        VkDeviceSize offset = 0;
        m_df->vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertices.buffer, &offset);
        m_df->vkCmdBindIndexBuffer(cmd, m_indices.buffer, 0,
                                   VK_INDEX_TYPE_UINT32);
        m_df->vkCmdDrawIndexed(cmd, m_index_count, 1, 0, 0, 0);
        if (messages)
            m_message_pending[f] = true;
    }
    m_df->vkCmdEndRenderPass(cmd);
    if (m_timestamps)
        m_df->vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                  m_timestamps, 2 * f + 1);

    // Frame timing (smoothed), only between frames drawn back to back
    // (otherwise it would include idle time).
    double dt  = seconds_since(last_frame);
    last_frame = Clock::now();
    {
        std::lock_guard<std::mutex> lock(m_view->m_mutex);
        ViewStats& st = m_view->m_stats;
        ++st.frames;
        st.pipeline_ready = m_pipeline != VK_NULL_HANDLE;
        if (m_view->m_last_frame_continuous && !program_changed) {
            st.frame_ms = st.frame_ms ? 0.9 * st.frame_ms + 0.1 * dt * 1000
                                      : dt * 1000;
            st.fps      = 1000.0 / st.frame_ms;
        }
    }

    m_view->frameReady();
    // Keep drawing while the shader animates (or a message readback is
    // pending, or we're asked to), otherwise only when something changes.
    bool pending = false;
    for (bool p : m_message_pending)
        pending |= p;
    bool continuous = (animate && !m_view->m_paused) || m_view->m_continuous;
    m_view->m_last_frame_continuous = continuous;
    if (!continuous) {
        std::lock_guard<std::mutex> lock(m_view->m_mutex);
        m_view->m_stats.fps      = 0;
        m_view->m_stats.frame_ms = 0;
    }
    if (continuous || pending)
        m_view->requestUpdate();
}

}  // namespace osltoyvk
