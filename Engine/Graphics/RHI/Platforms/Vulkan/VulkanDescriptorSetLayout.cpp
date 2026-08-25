/**
 * @file VulkanDescriptorSetLayout.cpp
 * @brief VulkanDescriptorSetLayout 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanDescriptorSetLayout.h"
#include "VulkanDevice.h"
#include "VulkanSampler.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>
#include <unordered_map>

namespace primal::graphics::rhi {

namespace {

VkDescriptorType ToVkDescriptorType(DescriptorType t) {
    switch (t) {
        case DescriptorType::Sampler:               return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorType::CombinedImageSampler:  return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorType::SampledImage:          return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorType::StorageImage:          return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorType::UniformTexelBuffer:    return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case DescriptorType::StorageTexelBuffer:    return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        case DescriptorType::UniformBuffer:         return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::StorageBuffer:         return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::UniformBufferDynamic:  return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        case DescriptorType::StorageBufferDynamic:  return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        case DescriptorType::InputAttachment:       return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        case DescriptorType::SampledDepthImage:     return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        default:                                    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

VkShaderStageFlags ToVkShaderStageFlags(ShaderStage s) {
    VkShaderStageFlags f = 0;
    const u32 v = static_cast<u32>(s);
    if (v & static_cast<u32>(ShaderStage::Vertex))   f |= VK_SHADER_STAGE_VERTEX_BIT;
    if (v & static_cast<u32>(ShaderStage::Pixel))    f |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (v & static_cast<u32>(ShaderStage::Geometry)) f |= VK_SHADER_STAGE_GEOMETRY_BIT;
    if (v & static_cast<u32>(ShaderStage::Hull))     f |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (v & static_cast<u32>(ShaderStage::Domain))   f |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    if (v & static_cast<u32>(ShaderStage::Compute))  f |= VK_SHADER_STAGE_COMPUTE_BIT;
    return f;
}

} // anonymous namespace

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(VulkanDevice& device, const DescriptorSetLayoutDesc& desc)
    : RHIDescriptorSetLayout(device, desc) {}

bool VulkanDescriptorSetLayout::Initialize() {
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();

    // === VkDescriptorSetLayout ===
    std::vector<VkDescriptorSetLayoutBinding> vkBindings;
    vkBindings.reserve(bindings_.size());
    // P4c-F8: immutable sampler 的 VkSampler 数组须存活到
    // vkCreateDescriptorSetLayout 返回 — 按绑定持久化
    std::vector<std::vector<VkSampler>> immutableSamplers;
    immutableSamplers.reserve(bindings_.size());
    for (const auto& b : bindings_) {
        VkDescriptorSetLayoutBinding vk{};
        vk.binding         = b.binding;
        vk.descriptorType  = ToVkDescriptorType(b.descriptorType);
        vk.descriptorCount = std::max<u32>(1u, b.descriptorCount);
        vk.stageFlags      = ToVkShaderStageFlags(b.stageFlags);
        vk.pImmutableSamplers = nullptr;
        // P4c-F8: 消费 ImmutableSampler 数组(Sampler/CombinedImageSampler 绑定
        // 有效;绑定后 descriptor write 不再需要 sampler)
        if (b.immutableSamplers != nullptr &&
            (b.descriptorType == DescriptorType::Sampler ||
             b.descriptorType == DescriptorType::CombinedImageSampler)) {
            immutableSamplers.emplace_back();
            auto& samplers = immutableSamplers.back();
            samplers.reserve(vk.descriptorCount);
            VulkanDevice& vkDev = static_cast<VulkanDevice&>(device_);
            for (u32 i = 0; i < vk.descriptorCount; ++i) {
                VulkanSampler* s = vkDev.GetSampler(b.immutableSamplers[i]);
                if (!s || s->GetNativeSampler() == VK_NULL_HANDLE) {
                    std::cerr << "[VulkanDescriptorSetLayout] immutable sampler "
                              << i << " invalid at binding " << b.binding
                              << " — falling back to mutable" << std::endl;
                    samplers.clear();
                    break;
                }
                samplers.push_back(s->GetNativeSampler());
            }
            if (!samplers.empty()) {
                vk.pImmutableSamplers = samplers.data();
            } else {
                immutableSamplers.pop_back();
            }
        } else {
            immutableSamplers.emplace_back();
        }
        vkBindings.push_back(vk);
    }

    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = static_cast<u32>(vkBindings.size());
    lci.pBindings    = vkBindings.data();

    // T6.3 sub-B: DescriptorBindingFlags → VkDescriptorSetLayoutBindingFlags。
    // 目前只映射 PartiallyBound(数组槽位未写合法;无需 UPDATE_AFTER_BIND 池)。
    // UpdateAfterBind/VariableDescriptorCount 需要池/分配侧配套,暂不消费,
    // 请求即告警忽略(避免静默拿到错误布局)。
    std::vector<VkDescriptorBindingFlags> bindingFlags;
    bool anyBindingFlags = false;
    for (const auto& b : bindings_) {
        VkDescriptorBindingFlags f = 0;
        const u32 raw = static_cast<u32>(b.flags);
        if (raw & static_cast<u32>(DescriptorBindingFlags::PartiallyBound)) {
            f |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }
        if ((raw & static_cast<u32>(DescriptorBindingFlags::UpdateAfterBind)) ||
            (raw & static_cast<u32>(DescriptorBindingFlags::UpdateUnusedWhilePending)) ||
            (raw & static_cast<u32>(DescriptorBindingFlags::VariableDescriptorCount))) {
            std::cerr << "[VulkanDescriptorSetLayout] binding " << b.binding
                      << " requests unsupported binding flag (UpdateAfterBind/"
                         "UpdateUnusedWhilePending/VariableDescriptorCount) — ignored"
                      << std::endl;
        }
        if (f != 0) anyBindingFlags = true;
        bindingFlags.push_back(f);
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsCI{};
    if (anyBindingFlags) {
        flagsCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsCI.bindingCount = static_cast<u32>(bindingFlags.size());
        flagsCI.pBindingFlags = bindingFlags.data();
        lci.pNext = &flagsCI;
    }

    if (vkCreateDescriptorSetLayout(dev, &lci, nullptr, &layout_) != VK_SUCCESS) {
        std::cerr << "[VulkanDescriptorSetLayout] vkCreateDescriptorSetLayout failed" << std::endl;
        return false;
    }

    // === Per-layout VkDescriptorPool ===
    // 按 type 聚合 pool sizes
    std::unordered_map<VkDescriptorType, u32> poolSizes;
    for (const auto& b : bindings_) {
        VkDescriptorType t = ToVkDescriptorType(b.descriptorType);
        poolSizes[t] += std::max<u32>(1u, b.descriptorCount);
    }
    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(poolSizes.size());
    // T4.6.5 part 35.6 → FSR-VK-GAP: 256 maxSets 在全场景材质集下耗尽
    // (Sponza 393 个 material_ds_ 同布局 — 第 257 个分配失败返回 INVALID,
    // draw 绑定失效 set → MoltenVK 惰性编码 bindDescriptorSets 段错误)。
    // Vulkan 池是容量预留而非预分配,扩到 4096 无实内存代价;
    // 耗尽路径改为显式错误(不再静默 INVALID)。
    static constexpr u32 kPoolMaxSets = 4096;
    for (const auto& kv : poolSizes) {
        sizes.push_back({ kv.first, kv.second * kPoolMaxSets });
    }

    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = kPoolMaxSets;
    pci.poolSizeCount = static_cast<u32>(sizes.size());
    pci.pPoolSizes    = sizes.data();

    if (vkCreateDescriptorPool(dev, &pci, nullptr, &pool_) != VK_SUCCESS) {
        std::cerr << "[VulkanDescriptorSetLayout] vkCreateDescriptorPool failed" << std::endl;
        vkDestroyDescriptorSetLayout(dev, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
        return false;
    }

    state_ = ResourceState::Ready;
    return true;
}

void VulkanDescriptorSetLayout::destroyImpl() {
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    VkDescriptorSetLayout l = layout_;
    VkDescriptorPool      p = pool_;
    if (l == VK_NULL_HANDLE && p == VK_NULL_HANDLE) return;

    device_.GetGarbageCollector().DeferredDestroy([dev, l, p]() {
        if (dev != VK_NULL_HANDLE && p != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, p, nullptr);
        if (dev != VK_NULL_HANDLE && l != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    });
    layout_ = VK_NULL_HANDLE;
    pool_   = VK_NULL_HANDLE;
}

VkDescriptorSet VulkanDescriptorSetLayout::AllocateSet() {
    if (layout_ == VK_NULL_HANDLE || pool_ == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout_;

    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) {
        // 尝试 reset pool(可能满了)— Phase 4 简化:不 reset,直接失败
        std::cerr << "[VulkanDescriptorSetLayout] vkAllocateDescriptorSets failed" << std::endl;
        return VK_NULL_HANDLE;
    }
    return set;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
