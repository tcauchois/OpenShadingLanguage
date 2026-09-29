// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include "vkcompute.h"

#include <cstring>

#include <slang/slang-com-ptr.h>
#include <slang/slang.h>
#include <vulkan/vulkan.h>

#include "oslspirv.h"


OSL_NAMESPACE_BEGIN

namespace spirv {

namespace {

std::string
vk_error(const char* what, VkResult result)
{
    return std::string(what) + " failed (VkResult " + std::to_string(result)
           + ")";
}



std::string
blob_string(slang::IBlob* blob)
{
    return blob ? std::string((const char*)blob->getBufferPointer(),
                              blob->getBufferSize())
                : std::string();
}

}  // namespace



struct VulkanCompute::Impl {
    VkInstance instance            = VK_NULL_HANDLE;
    VkPhysicalDevice physical      = VK_NULL_HANDLE;
    VkDevice device                = VK_NULL_HANDLE;
    VkQueue queue                  = VK_NULL_HANDLE;
    uint32_t queue_family          = 0;
    VkCommandPool command_pool     = VK_NULL_HANDLE;
    VkDescriptorSetLayout dset_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout  = VK_NULL_HANDLE;
    VkDescriptorPool dpool         = VK_NULL_HANDLE;
    VkDescriptorSet dset           = VK_NULL_HANDLE;
    VkShaderModule shader          = VK_NULL_HANDLE;
    VkPipeline pipeline            = VK_NULL_HANDLE;
    struct Buffer {
        VkBuffer buffer       = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        size_t size           = 0;
    };
    Buffer buffers[max_buffers];
    std::string device_name;
    bool anisotropy = false;

    struct Texture {
        VkImage image         = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view      = VK_NULL_HANDLE;
    };
    std::vector<Texture> textures;
    Texture black;  // fills the unused texture slots
    VkSampler samplers[num_samplers] = {};

    static constexpr uint32_t max_push_size = 128;

    void destroy_pipeline()
    {
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (shader)
            vkDestroyShaderModule(device, shader, nullptr);
        pipeline = VK_NULL_HANDLE;
        shader   = VK_NULL_HANDLE;
    }

    void destroy_buffer(Buffer& b)
    {
        if (b.buffer)
            vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory)
            vkFreeMemory(device, b.memory, nullptr);
        b = Buffer();
    }

    void destroy_texture(Texture& t)
    {
        if (t.view)
            vkDestroyImageView(device, t.view, nullptr);
        if (t.image)
            vkDestroyImage(device, t.image, nullptr);
        if (t.memory)
            vkFreeMemory(device, t.memory, nullptr);
        t = Texture();
    }

    ~Impl()
    {
        if (device) {
            vkDeviceWaitIdle(device);
            destroy_pipeline();
            for (Buffer& b : buffers)
                destroy_buffer(b);
            for (Texture& t : textures)
                destroy_texture(t);
            destroy_texture(black);
            for (VkSampler s : samplers)
                if (s)
                    vkDestroySampler(device, s, nullptr);
            if (dpool)
                vkDestroyDescriptorPool(device, dpool, nullptr);
            if (pipeline_layout)
                vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (dset_layout)
                vkDestroyDescriptorSetLayout(device, dset_layout, nullptr);
            if (command_pool)
                vkDestroyCommandPool(device, command_pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }

    bool memory_type(uint32_t type_bits, VkMemoryPropertyFlags want,
                     uint32_t& type) const
    {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        for (type = 0; type < props.memoryTypeCount; ++type)
            if ((type_bits & (1u << type))
                && (props.memoryTypes[type].propertyFlags & want) == want)
                return true;
        return false;
    }

    // Run commands recorded by `record` and wait for them.
    template<typename F> bool run_commands(F record, std::string& err)
    {
        VkCommandBufferAllocateInfo cbai {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
        };
        cbai.commandPool        = command_pool;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cb;
        vkAllocateCommandBuffers(device, &cbai, &cb);
        VkCommandBufferBeginInfo begin {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
        };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &begin);
        record(cb);
        vkEndCommandBuffer(cb);
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cb;
        VkResult r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        if (r == VK_SUCCESS)
            r = vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(device, command_pool, 1, &cb);
        if (r != VK_SUCCESS) {
            err = vk_error("vkQueueSubmit", r);
            return false;
        }
        return true;
    }

    bool create_texture(int width, int height,
                        const std::vector<std::vector<float>>& levels,
                        Texture& t, std::string& err);
    void bind_textures();

    bool ensure_buffer(int index, size_t size, std::string& err)
    {
        Buffer& b = buffers[index];
        size      = std::max(size, size_t(16));
        if (size <= b.size)
            return true;
        destroy_buffer(b);
        VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkResult r = vkCreateBuffer(device, &bci, nullptr, &b.buffer);
        if (r != VK_SUCCESS) {
            err = vk_error("vkCreateBuffer", r);
            return false;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        uint32_t type = 0;
        if (!memory_type(req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                             | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         type)) {
            err = "no host-visible memory type for the storage buffer";
            return false;
        }
        VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = type;
        if ((r = vkAllocateMemory(device, &mai, nullptr, &b.memory))
            != VK_SUCCESS) {
            err = vk_error("vkAllocateMemory", r);
            return false;
        }
        vkBindBufferMemory(device, b.buffer, b.memory, 0);
        b.size = size;

        VkDescriptorBufferInfo dbi { b.buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet write { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = dset;
        write.dstBinding      = uint32_t(index);
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = &dbi;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        return true;
    }
};



bool
VulkanCompute::Impl::create_texture(
    int width, int height, const std::vector<std::vector<float>>& levels,
    Texture& t, std::string& err)
{
    VkImageCreateInfo ici { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = VK_FORMAT_R32G32B32A32_SFLOAT;
    ici.extent      = { uint32_t(width), uint32_t(height), 1 };
    ici.mipLevels   = uint32_t(levels.size());
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r        = vkCreateImage(device, &ici, nullptr, &t.image);
    if (r != VK_SUCCESS) {
        err = vk_error("vkCreateImage", r);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, t.image, &req);
    VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    if (!memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     mai.memoryTypeIndex)
        && !memory_type(req.memoryTypeBits, 0, mai.memoryTypeIndex)) {
        err = "no memory type for the texture";
        return false;
    }
    if ((r = vkAllocateMemory(device, &mai, nullptr, &t.memory)) != VK_SUCCESS) {
        err = vk_error("vkAllocateMemory", r);
        return false;
    }
    vkBindImageMemory(device, t.image, t.memory, 0);

    // Copy the levels through a staging buffer.
    size_t total = 0;
    for (auto& level : levels)
        total += level.size() * sizeof(float);
    Buffer staging;
    VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = std::max(total, size_t(16));
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(device, &bci, nullptr, &staging.buffer);
    vkGetBufferMemoryRequirements(device, staging.buffer, &req);
    mai.allocationSize = req.size;
    if (!memory_type(req.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     mai.memoryTypeIndex)) {
        destroy_buffer(staging);
        err = "no host-visible memory type for the staging buffer";
        return false;
    }
    vkAllocateMemory(device, &mai, nullptr, &staging.memory);
    vkBindBufferMemory(device, staging.buffer, staging.memory, 0);
    void* mapped = nullptr;
    vkMapMemory(device, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    std::vector<VkBufferImageCopy> copies;
    size_t offset = 0;
    for (size_t l = 0; l < levels.size(); ++l) {
        memcpy((char*)mapped + offset, levels[l].data(),
               levels[l].size() * sizeof(float));
        VkBufferImageCopy c {};
        c.bufferOffset     = offset;
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(l), 0, 1 };
        c.imageExtent      = { uint32_t(std::max(1, width >> l)),
                               uint32_t(std::max(1, height >> l)), 1 };
        copies.push_back(c);
        offset += levels[l].size() * sizeof(float);
    }
    vkUnmapMemory(device, staging.memory);

    VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                    uint32_t(levels.size()), 0, 1 };
    bool ok = run_commands(
        [&](VkCommandBuffer cb) {
            VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = t.image;
            b.subresourceRange    = range;
            b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
            vkCmdCopyBufferToImage(cb, staging.buffer, t.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   uint32_t(copies.size()), copies.data());
            b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
        },
        err);
    destroy_buffer(staging);
    if (!ok)
        return false;

    VkImageViewCreateInfo vci { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image            = t.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = ici.format;
    vci.subresourceRange = range;
    if ((r = vkCreateImageView(device, &vci, nullptr, &t.view)) != VK_SUCCESS) {
        err = vk_error("vkCreateImageView", r);
        return false;
    }
    return true;
}



void
VulkanCompute::Impl::bind_textures()
{
    VkDescriptorImageInfo images[max_textures];
    for (int i = 0; i < max_textures; ++i)
        images[i] = { VK_NULL_HANDLE,
                      i < int(textures.size()) ? textures[i].view : black.view,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo sampler_infos[num_samplers];
    for (int i = 0; i < num_samplers; ++i)
        sampler_infos[i] = { samplers[i], VK_NULL_HANDLE,
                             VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet writes[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
    };
    writes[0].dstSet          = dset;
    writes[0].dstBinding      = max_buffers;
    writes[0].descriptorCount = max_textures;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo      = images;
    writes[1].dstSet          = dset;
    writes[1].dstBinding      = max_buffers + 1;
    writes[1].descriptorCount = num_samplers;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo      = sampler_infos;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}



VulkanCompute::VulkanCompute()
    : m_impl(new Impl)
{
}



VulkanCompute::~VulkanCompute() {}



const std::string&
VulkanCompute::device_name() const
{
    return m_impl->device_name;
}



bool
VulkanCompute::init(std::string& err)
{
    Impl& vk = *m_impl;
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "OSL";
    app.apiVersion       = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
#ifdef __APPLE__
    // Vulkan on macOS is provided by "portability" drivers.
    const char* instance_exts[] = {
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    };
    ici.flags                   = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ici.enabledExtensionCount   = 1;
    ici.ppEnabledExtensionNames = instance_exts;
#endif
    VkResult r = vkCreateInstance(&ici, nullptr, &vk.instance);
    if (r != VK_SUCCESS) {
        err = vk_error("vkCreateInstance", r);
        return false;
    }

    uint32_t ndevices = 0;
    vkEnumeratePhysicalDevices(vk.instance, &ndevices, nullptr);
    std::vector<VkPhysicalDevice> devices(ndevices);
    vkEnumeratePhysicalDevices(vk.instance, &ndevices, devices.data());
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3)
            continue;
        uint32_t nfamilies = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nfamilies, nullptr);
        std::vector<VkQueueFamilyProperties> families(nfamilies);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nfamilies,
                                                 families.data());
        for (uint32_t f = 0; f < nfamilies; ++f) {
            if (families[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                vk.physical     = pd;
                vk.queue_family = f;
                vk.device_name  = props.deviceName;
                break;
            }
        }
        if (vk.physical)
            break;
    }
    if (!vk.physical) {
        err = "no Vulkan 1.3 device with compute support";
        return false;
    }

    // OSL code uses 64-bit ints (string hashes, opaque handles) and 8-bit
    // bools in groupdata.
    VkPhysicalDeviceVulkan12Features f12 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
    };
    f12.shaderInt8 = VK_TRUE;
    // Texture indices may vary between invocations.
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures2 features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features.pNext                 = &f12;
    features.features.shaderInt64  = VK_TRUE;
    features.features.shaderInt16  = VK_TRUE;
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(vk.physical, &supported);
    vk.anisotropy = supported.samplerAnisotropy;
    features.features.samplerAnisotropy = supported.samplerAnisotropy;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = vk.queue_family;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext                = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;

    // Portability drivers require enabling VK_KHR_portability_subset.
    std::vector<const char*> device_exts;
    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &next, nullptr);
    std::vector<VkExtensionProperties> exts(next);
    vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &next,
                                         exts.data());
    for (auto& e : exts)
        if (!strcmp(e.extensionName, "VK_KHR_portability_subset"))
            device_exts.push_back("VK_KHR_portability_subset");
    dci.enabledExtensionCount   = uint32_t(device_exts.size());
    dci.ppEnabledExtensionNames = device_exts.data();

    if ((r = vkCreateDevice(vk.physical, &dci, nullptr, &vk.device))
        != VK_SUCCESS) {
        err = vk_error("vkCreateDevice", r);
        return false;
    }
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);

    VkCommandPoolCreateInfo cpci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = vk.queue_family;
    vkCreateCommandPool(vk.device, &cpci, nullptr, &vk.command_pool);

    const int nbindings                              = max_buffers + 2;
    VkDescriptorSetLayoutBinding bindings[nbindings] = {};
    for (int i = 0; i < nbindings; ++i) {
        bindings[i].binding         = uint32_t(i);
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[max_buffers].descriptorType      = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[max_buffers].descriptorCount     = max_textures;
    bindings[max_buffers + 1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[max_buffers + 1].descriptorCount = num_samplers;
    VkDescriptorSetLayoutCreateInfo dslci {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    dslci.bindingCount = nbindings;
    dslci.pBindings    = bindings;
    vkCreateDescriptorSetLayout(vk.device, &dslci, nullptr, &vk.dset_layout);

    VkPushConstantRange push { VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               Impl::max_push_size };
    VkPipelineLayoutCreateInfo plci { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &vk.dset_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &push;
    vkCreatePipelineLayout(vk.device, &plci, nullptr, &vk.pipeline_layout);

    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, max_buffers },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, max_textures },
        { VK_DESCRIPTOR_TYPE_SAMPLER, num_samplers },
    };
    VkDescriptorPoolCreateInfo dpci { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = 1;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes    = pool_sizes;
    vkCreateDescriptorPool(vk.device, &dpci, nullptr, &vk.dpool);
    VkDescriptorSetAllocateInfo dsai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = vk.dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &vk.dset_layout;
    vkAllocateDescriptorSets(vk.device, &dsai, &vk.dset);

    // Samplers for each combination of wrap modes and filtering.
    const VkSamplerAddressMode modes[4] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,           // WrapPeriodic
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,  // WrapBlack
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,    // WrapClamp
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,  // WrapMirror
    };
    for (int s = 0; s < 4; ++s)
        for (int t = 0; t < 4; ++t)
            for (int nearest = 0; nearest < 2; ++nearest) {
                VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                sci.magFilter  = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
                sci.minFilter  = sci.magFilter;
                sci.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                         : VK_SAMPLER_MIPMAP_MODE_LINEAR;
                sci.addressModeU     = modes[s];
                sci.addressModeV     = modes[t];
                sci.addressModeW     = modes[t];
                sci.anisotropyEnable = vk.anisotropy && !nearest;
                sci.maxAnisotropy    = 16.0f;
                sci.minLod           = 0.0f;
                sci.maxLod           = VK_LOD_CLAMP_NONE;
                sci.borderColor      = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                r = vkCreateSampler(vk.device, &sci, nullptr,
                                    &vk.samplers[sampler_index(s, t, nearest)]);
                if (r != VK_SUCCESS) {
                    err = vk_error("vkCreateSampler", r);
                    return false;
                }
            }
    if (!vk.create_texture(1, 1, { { 0.0f, 0.0f, 0.0f, 0.0f } }, vk.black,
                           err))
        return false;
    vk.bind_textures();
    return true;
}



int
VulkanCompute::add_texture(int width, int height,
                           const std::vector<std::vector<float>>& levels,
                           std::string& err)
{
    Impl& vk = *m_impl;
    if (int(vk.textures.size()) >= max_textures) {
        err = "too many textures";
        return -1;
    }
    Impl::Texture t;
    if (!vk.create_texture(width, height, levels, t, err)) {
        vk.destroy_texture(t);
        return -1;
    }
    vk.textures.push_back(t);
    vk.bind_textures();
    return int(vk.textures.size()) - 1;
}



void
VulkanCompute::clear_textures()
{
    Impl& vk = *m_impl;
    vkDeviceWaitIdle(vk.device);
    for (Impl::Texture& t : vk.textures)
        vk.destroy_texture(t);
    vk.textures.clear();
    vk.bind_textures();
}



bool
VulkanCompute::compile_slang(
    const std::string& source,
    const std::vector<std::pair<std::string, std::string>>& include_sources,
    const std::vector<std::string>& imports, std::vector<uint32_t>& spirv,
    std::string& err)
{
    static Slang::ComPtr<slang::IGlobalSession> global_session;
    if (!global_session
        && SLANG_FAILED(slang::createGlobalSession(global_session.writeRef()))) {
        err = "could not create Slang global session";
        return false;
    }

    slang::TargetDesc target {};
    target.format  = SLANG_SPIRV;
    target.profile = global_session->findProfile("spirv_1_6");
    slang::CompilerOptionEntry options[] = {
        // Don't fail on the [__extern] functions we will link later.
        { slang::CompilerOptionName::IncompleteLibrary,
          { slang::CompilerOptionValueKind::Int, 1 } },
    };
    slang::SessionDesc sdesc {};
    sdesc.targets                  = &target;
    sdesc.targetCount              = 1;
    sdesc.compilerOptionEntries    = options;
    sdesc.compilerOptionEntryCount = 1;
    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global_session->createSession(sdesc, session.writeRef()))) {
        err = "could not create Slang session";
        return false;
    }

    // Load the includes as modules the kernel can `import`.
    std::vector<slang::IComponentType*> components;
    Slang::ComPtr<slang::IBlob> diagnostics;
    for (auto&& inc : include_sources) {
        slang::IModule* m = session->loadModuleFromSourceString(
            inc.first.c_str(), (inc.first + ".slang").c_str(),
            inc.second.c_str(), diagnostics.writeRef());
        if (!m) {
            err = "Slang: " + blob_string(diagnostics);
            return false;
        }
        components.push_back(m);
    }
    slang::IModule* kernel = session->loadModuleFromSourceString(
        "osl_kernel", "osl_kernel.slang", source.c_str(),
        diagnostics.writeRef());
    if (!kernel) {
        err = "Slang: " + blob_string(diagnostics);
        return false;
    }
    Slang::ComPtr<slang::IEntryPoint> entry;
    if (SLANG_FAILED(kernel->findEntryPointByName("main", entry.writeRef()))) {
        err = "Slang: kernel has no entry point \"main\"";
        return false;
    }
    components.push_back(kernel);
    components.push_back(entry);

    Slang::ComPtr<slang::IComponentType> composite, linked;
    Slang::ComPtr<slang::IBlob> code;
    if (SLANG_FAILED(session->createCompositeComponentType(
            components.data(), components.size(), composite.writeRef(),
            diagnostics.writeRef()))
        || SLANG_FAILED(composite->link(linked.writeRef(),
                                        diagnostics.writeRef()))
        || SLANG_FAILED(linked->getTargetCode(0, code.writeRef(),
                                              diagnostics.writeRef()))) {
        err = "Slang: " + blob_string(diagnostics);
        return false;
    }
    spirv.resize(code->getBufferSize() / sizeof(uint32_t));
    memcpy(spirv.data(), code->getBufferPointer(),
           spirv.size() * sizeof(uint32_t));
    return imports.empty() || make_imports(spirv, imports, err);
}



bool
VulkanCompute::set_pipeline(const std::vector<uint32_t>& spirv,
                            std::string& err)
{
    Impl& vk = *m_impl;
    vk.destroy_pipeline();
    VkShaderModuleCreateInfo smci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = spirv.size() * sizeof(uint32_t);
    smci.pCode    = spirv.data();
    VkResult r    = vkCreateShaderModule(vk.device, &smci, nullptr, &vk.shader);
    if (r != VK_SUCCESS) {
        err = vk_error("vkCreateShaderModule", r);
        return false;
    }
    VkComputePipelineCreateInfo cpci { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = vk.shader;
    cpci.stage.pName  = "main";
    cpci.layout       = vk.pipeline_layout;
    r = vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                 &vk.pipeline);
    if (r != VK_SUCCESS) {
        err = vk_error("vkCreateComputePipelines", r);
        return false;
    }
    return true;
}



bool
VulkanCompute::dispatch(uint32_t xgroups, uint32_t ygroups,
                        const void* push_data, uint32_t push_size,
                        std::vector<std::vector<uint8_t>>& buffers,
                        std::string& err)
{
    Impl& vk = *m_impl;
    if (!vk.pipeline) {
        err = "no pipeline";
        return false;
    }
    if (push_size > Impl::max_push_size) {
        err = "too many push constants";
        return false;
    }
    if (buffers.size() > size_t(max_buffers)) {
        err = "too many buffers";
        return false;
    }
    // Every binding must have a buffer, even if the shader doesn't use it.
    for (int i = 0; i < max_buffers; ++i) {
        size_t size = i < int(buffers.size()) ? buffers[i].size() : 0;
        if (!vk.ensure_buffer(i, size, err))
            return false;
        void* mapped = nullptr;
        vkMapMemory(vk.device, vk.buffers[i].memory, 0, VK_WHOLE_SIZE, 0,
                    &mapped);
        if (size)
            memcpy(mapped, buffers[i].data(), size);
        vkUnmapMemory(vk.device, vk.buffers[i].memory);
    }

    bool ok = vk.run_commands(
        [&](VkCommandBuffer cb) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, vk.pipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    vk.pipeline_layout, 0, 1, &vk.dset, 0,
                                    nullptr);
            if (push_size)
                vkCmdPushConstants(cb, vk.pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size,
                                   push_data);
            vkCmdDispatch(cb, xgroups, ygroups, 1);
        },
        err);
    if (!ok)
        return false;

    for (size_t i = 0; i < buffers.size(); ++i) {
        void* mapped = nullptr;
        vkMapMemory(vk.device, vk.buffers[i].memory, 0, VK_WHOLE_SIZE, 0,
                    &mapped);
        memcpy(buffers[i].data(), mapped, buffers[i].size());
        vkUnmapMemory(vk.device, vk.buffers[i].memory);
    }
    return true;
}

}  // namespace spirv

OSL_NAMESPACE_END
