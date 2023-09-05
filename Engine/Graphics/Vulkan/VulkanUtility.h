#pragma once

#include "VulkanCommonHeaders.h"

#include "VulkanCore.h"
#include "VulkanHelpers.h"

namespace primal::graphics::vulkan
{
    // For easier to finish setting some setup(e.p. descriptor, sampler, image...)
    // T: Vulkan Handle(e.p. VkPipeline, VkImage, VkSampler, VkImageView...)
    // P: Structure to create handle(e.p. VkPipelineCreateinfo, VkImageCreateInfo, VkImageViewCreateInfo...)
    template<typename T, typename P>
    class vulkan_base_class
    {
    public:
        virtual void setter(P p) = 0;

        virtual T getter() = 0;

        virtual void compile() = 0;
        virtual void release() = 0;

    protected:
        T                                   value;
        P                                   info;
    };

#define PE(name) vulkan_base_class<name, name##CreateInfo>
#define PE_(name, expression) class PE_Vk##name : public PE(Vk##name)                       \
{                                                                                           \
public:                                                                                     \
    PE_Vk##name() = default;                                                                \
    ~PE_Vk##name() { this->release(); }                                                     \
    virtual void setter(Vk##name##CreateInfo i) override                                    \
    {                                                                                       \
        this->info = i;                                                                     \
    }                                                                                       \
    virtual Vk##name getter() override { return this->value;}                               \
                                                                                            \
    virtual void compile() override                                                         \
    {                                                                                       \
        expression;                                                                         \
    }                                                                                       \
    virtual void release() override                                                         \
    {                                                                                       \
        vkDestroy##name(core::logical_device(), this->value, nullptr);                      \
    }                                                                                       \
}

    typedef PE_(Sampler, vkCreateSampler(core::logical_device(), &this->info, nullptr, &this->value))                                   PE_VkSampler;
    typedef PE_(Image, vkCreateImage(core::logical_device(), &this->info, nullptr, &this->value))                                       PE_VkImage;
    typedef PE_(ImageView, vkCreateImageView(core::logical_device(), &this->info, nullptr, &this->value))                               PE_VkImageView;
    typedef PE_(DescriptorPool, vkCreateDescriptorPool(core::logical_device(), &this->info, nullptr, &this->value))                     PE_VkDescriptorPool;
    typedef PE_(DescriptorSetLayout, vkCreateDescriptorSetLayout(core::logical_device(), &this->info, nullptr, &this->value))           PE_VkDescriptorSetLayout;
    typedef PE_(PipelineLayout, vkCreatePipelineLayout(core::logical_device(), &this->info, nullptr, &this->value))                     PE_VkPipelineLayout;
    typedef PE_(PipelineCache, vkCreatePipelineCache(core::logical_device(), &this->info, nullptr, &this->value))                       PE_VkPipelineCache;
    
    struct PE_vk_attachment_description
    {
        VkAttachmentDescription info;
        PE_vk_attachment_description() : info
        {
            0, VK_FORMAT_R32G32B32A32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
        } {}
    };


    struct PE_vk_subpass_dependency
    {
        VkSubpassDependency info;
        PE_vk_subpass_dependency() : info
        {
            VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0
        } {}
    };

    struct PE_vk_attachment_reference
    {
        VkAttachmentReference info;
        PE_vk_attachment_reference(u32 attachment) : info
        {
            attachment, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        } {}
    };

    struct PE_vk_subpass_description
    {
        VkSubpassDescription info;
        PE_vk_subpass_description() : info
        {
            0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 0
        } {}
    };

    struct PE_vk_renderpass_info
    {
        VkRenderPassCreateInfo info;
        PE_vk_renderpass_info() : info
        {
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 0, nullptr, 0, nullptr, 0, nullptr
        } {}
    };

    struct PE_vk_image_info
    {
        VkImageCreateInfo info;
        PE_vk_image_info() :info
        {
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {800, 600, 1}, 0, 0, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR,
            VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        } {}
    };

    struct PE_vk_image_view_info
    {
        VkImageViewCreateInfo info;
        PE_vk_image_view_info() : info
        {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, nullptr, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT,
            { VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE }, VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1, 0
        } {}
        PE_vk_image_view_info(VkImage image) : info
        {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT,
            { VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE }, VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1, 0
        } {}
        PE_vk_image_view_info(VkImage image, VkImageAspectFlags aspectMask) : info
        {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT,
            { VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE }, aspectMask, 1, 0, 1, 0
        } {}
        PE_vk_image_view_info(VkImage image, VkFormat format, VkImageAspectFlags aspectMask) : info
        {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image, VK_IMAGE_VIEW_TYPE_2D, format,
            { VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE }, aspectMask, 1, 0, 1, 0
        } {}
    };

    struct PE_vk_image_sampler_info
    {
        VkSamplerCreateInfo info;
        PE_vk_image_sampler_info() : info
        {
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT,           
            VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0.f, VK_TRUE, 1.f, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0.f, 1.f, VK_BORDER_COLOR_INT_OPAQUE_BLACK, VK_FALSE                                 
        }{}
    };

    struct PE_vk_framebuffer_info
    {
        VkFramebufferCreateInfo info;
        PE_vk_framebuffer_info(VkRenderPass pass, u32 width, u32 height) : info
        {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, pass, 0, nullptr, width, height, 0
        } {}
    };

    struct PE_vk_descriptor_pool_size
    {
        VkDescriptorPoolSize info;
        PE_vk_descriptor_pool_size(VkDescriptorType type, u32 count) : info
        {
            type, count
        } {}
    };

    struct PE_vk_descriptor_pool_info
    {
        VkDescriptorPoolCreateInfo info;
        PE_vk_descriptor_pool_info(utl::vector<VkDescriptorPoolSize> poolSize) : info
        {
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 48, static_cast<u32>(poolSize.size()), poolSize.data()
        } {}
    };

    struct PE_vk_descriptor_set_layout_binding
    {
        VkDescriptorSetLayoutBinding info;
        PE_vk_descriptor_set_layout_binding(u32 binding, VkDescriptorType type, VkShaderStageFlags flags) : info
        {
            binding, type, 1, flags, nullptr
        } {}
    };

    struct PE_vk_descriptor_set_layout_info
    {
        VkDescriptorSetLayoutCreateInfo info;
        PE_vk_descriptor_set_layout_info(utl::vector<VkDescriptorSetLayoutBinding> bindings) : info
        {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, static_cast<u32>(bindings.size()), bindings.data()
        } {}
    };

    struct PE_vk_pipeline_layout_info
    {
        VkPipelineLayoutCreateInfo info;
        PE_vk_pipeline_layout_info(utl::vector<VkDescriptorSetLayout> ds) : info
        {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, static_cast<u32>(ds.size()), ds.data(), 0, nullptr
        } {}
    };

    struct PE_vk_pipeline_input_assemble_info
    {
        VkPipelineInputAssemblyStateCreateInfo info;
        PE_vk_pipeline_input_assemble_info() : info
        {
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE
        } {}
    };

    struct PE_vk_pipeline_viewport_info
    {
        VkPipelineViewportStateCreateInfo info;
        PE_vk_pipeline_viewport_info() : info
        {
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr
        } {}
    };

    struct PE_vk_pipeline_raster_info
    {
        VkPipelineRasterizationStateCreateInfo info;
        PE_vk_pipeline_raster_info() : info
        {
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE,
            VK_FALSE, 0.f, 0.f, 0.f, 0.f
        } {}
    };

    struct PE_vk_pipeline_multisample_info
    {
        VkPipelineMultisampleStateCreateInfo info;
        PE_vk_pipeline_multisample_info() : info
        {
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0.f, nullptr, VK_FALSE, VK_FALSE
        } {}
    };

    struct PE_vk_pipeline_color_blend_attachemnt
    {
        VkPipelineColorBlendAttachmentState info;
        PE_vk_pipeline_color_blend_attachemnt() : info
        {
            VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xf
        } {}
    };

    struct PE_vk_pipeline_color_blend_info
    {
        VkPipelineColorBlendStateCreateInfo info;
        PE_vk_pipeline_color_blend_info(utl::vector<VkPipelineColorBlendAttachmentState> attaches) : info
        {
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, static_cast<u32>(attaches.size()), attaches.data(), {0.f, 0.f, 0.f, 0.f}
        } {}
    };

    struct PE_vk_pipeline_dynamic_info
    {
        VkPipelineDynamicStateCreateInfo info;
        PE_vk_pipeline_dynamic_info(utl::vector<VkDynamicState> d) : info
        {
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, static_cast<u32>(d.size()), d.data()
        } {}
    };

    struct PE_vk_pipeline_depth_stencil_info
    {
        VkPipelineDepthStencilStateCreateInfo info;
        PE_vk_pipeline_depth_stencil_info() : info
        {
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, nullptr, 0, VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS, VK_FALSE, VK_FALSE, {}, {}, 0.f, 1.f
        } {}
    };

    struct PE_vk_pipeline_vertex_input_info
    {
        VkPipelineVertexInputStateCreateInfo info;
        PE_vk_pipeline_vertex_input_info() : info
        {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0
        } 
        {
            auto binds = getVertexInputBindDescriptor();
            auto attrs = getVertexInputAttributeDescriptor();
            this->info.vertexBindingDescriptionCount = static_cast<u32>(binds.size());
            this->info.pVertexBindingDescriptions = binds.data();
            this->info.vertexAttributeDescriptionCount = static_cast<u32>(attrs.size());
            this->info.pVertexAttributeDescriptions = attrs.data();
        }
    };

    struct PE_vk_pipeline_graphics_info
    {
        VkGraphicsPipelineCreateInfo info;
        PE_vk_pipeline_graphics_info(utl::vector<VkPipelineShaderStageCreateInfo> shaderStages, VkPipelineLayout layout, VkRenderPass pass) : info
        {
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr, 0, static_cast<u32>(shaderStages.size()), shaderStages.data()
        } 
        {
            this->info.layout = layout;
            this->info.renderPass = pass;
            this->info.subpass = 0;
            this->info.basePipelineHandle = nullptr;
            this->info.basePipelineIndex = -1;
        }
    };
}