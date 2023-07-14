#pragma once

#include "VulkanCommonHeaders.h"

#include "VulkanCore.h"

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
    
}