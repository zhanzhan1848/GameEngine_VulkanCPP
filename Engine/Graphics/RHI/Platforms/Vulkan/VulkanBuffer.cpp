/**
 * @file VulkanBuffer.cpp
 * @brief VulkanBuffer 实现
 * @details VMA 集成模式:
 *          - Dynamic/Staging/Readback: VMA_MEMORY_USAGE_AUTO + HOST_VISIBLE,persistent-map
 *          - Static/Immutable:         VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE + DEVICE_LOCAL
 *          UpdateData 静态缓冲走 VulkanStagingAllocator 异步 blit 路径。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanBuffer.h"
#include "VulkanDevice.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
// VMA_IMPLEMENTATION 定义在 VulkanDevice.cpp 单 TU 内,这里仅引用声明
#include <vk_mem_alloc.h>
#endif

namespace primal::graphics::rhi {

// 辅助:BufferType → ResourceUsage(Metal 镜像)
static ResourceUsage GetResourceUsageFromBufferType(BufferType type, u32 /*bindFlags*/) {
    switch (type) {
        case BufferType::Vertex:        return ResourceUsage::VertexBuffer;
        case BufferType::Index:         return ResourceUsage::IndexBuffer;
        case BufferType::Constant:      return ResourceUsage::ConstantBuffer;
        case BufferType::Structured:    return ResourceUsage::ShaderResource | ResourceUsage::UnorderedAccess;
        case BufferType::Indirect:      return ResourceUsage::IndirectArg;
        case BufferType::Raw:           return ResourceUsage::ShaderResource | ResourceUsage::UnorderedAccess
                                                       | ResourceUsage::CopySource | ResourceUsage::CopyDest;
        default:                        return ResourceUsage::None;
    }
}

namespace {

// BufferType → VkBufferUsageFlags 转换(独立函数,供构造和 Initialize 复用)
VkBufferUsageFlags BufferTypeToVkUsage(BufferType type) {
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                             | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    switch (type) {
        case BufferType::Vertex:    usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; break;
        case BufferType::Index:     usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
        case BufferType::Constant:  usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
        case BufferType::Structured:
        case BufferType::Raw:       usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; break;
        case BufferType::Indirect:  usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT; break;
        default: break;
    }
    return usage;
}

} // anonymous namespace

VulkanBuffer::VulkanBuffer(VulkanDevice& device, const BufferDesc& desc)
    : RHIResource(device, ResourceDesc(
          ResourceType::Buffer,
          GetResourceUsageFromBufferType(desc.type, desc.bindFlags),
          desc.memoryUsage,
          desc.size,
          desc.name.c_str())),
      vkUsageFlags_(BufferTypeToVkUsage(desc.type))
{
}

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) noexcept
    : RHIResource(std::move(other)),
      vkBuffer_(other.vkBuffer_),
      allocation_(other.allocation_),
      persistentMappedPtr_(other.persistentMappedPtr_),
      vkUsageFlags_(other.vkUsageFlags_) {
    other.vkBuffer_ = VK_NULL_HANDLE;
    other.allocation_ = nullptr;
    other.persistentMappedPtr_ = nullptr;
    other.vkUsageFlags_ = 0;
}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept {
    if (this != &other) {
        destroyImpl();
        RHIResource::operator=(std::move(other));
        vkBuffer_ = other.vkBuffer_;
        allocation_ = other.allocation_;
        persistentMappedPtr_ = other.persistentMappedPtr_;
        vkUsageFlags_ = other.vkUsageFlags_;
        other.vkBuffer_ = VK_NULL_HANDLE;
        other.allocation_ = nullptr;
        other.persistentMappedPtr_ = nullptr;
        other.vkUsageFlags_ = 0;
    }
    return *this;
}

VulkanBuffer::~VulkanBuffer() {
    destroyImpl();
}

bool VulkanBuffer::Initialize() {
    if (desc_.size == 0) {
        std::cerr << "[VulkanBuffer] Initialize failed: size is 0" << std::endl;
        return false;
    }

    VulkanDevice& vkDevice = static_cast<VulkanDevice&>(device_);
    VmaAllocator allocator = vkDevice.GetVmaAllocator();
    if (!allocator) {
        std::cerr << "[VulkanBuffer] VMA allocator not initialized" << std::endl;
        return false;
    }

    // === Buffer usage flags(已在构造时缓存)===
    VkBufferUsageFlags usage = vkUsageFlags_;

    // === VMA allocation create flags ===
    VmaAllocationCreateFlags allocFlags = 0;
    VmaMemoryUsage vmaUsage = VMA_MEMORY_USAGE_AUTO;

    switch (desc_.memoryUsage) {
        case GPUMemoryUsage::Dynamic:
        case GPUMemoryUsage::Staging:
            // CPU 写 → GPU 读:host-visible + persistent-map(避免反复 Map/Unmap)
            allocFlags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                        | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            vmaUsage = VMA_MEMORY_USAGE_AUTO;
            break;
        case GPUMemoryUsage::Readback:
            // GPU 写 → CPU 读:host-visible + random access(因为 CPU 读取模式不可预测)
            allocFlags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                        | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            vmaUsage = VMA_MEMORY_USAGE_AUTO;
            break;
        case GPUMemoryUsage::Static:
        case GPUMemoryUsage::Immutable:
        default:
            // GPU-only:device-local,CPU 不可见,UpdateData 走 staging blit
            vmaUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            break;
    }

    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = desc_.size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.flags = 0;

    VmaAllocationCreateInfo allocCI{};
    allocCI.flags = allocFlags;
    allocCI.usage = vmaUsage;
    allocCI.memoryTypeBits = 0;
    allocCI.pool = nullptr;
    allocCI.pUserData = nullptr;
    allocCI.priority = 0.5f;

    VmaAllocationInfo allocInfo{};
    VkResult res = vmaCreateBuffer(allocator, &ci, &allocCI, &vkBuffer_, &allocation_, &allocInfo);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanBuffer] vmaCreateBuffer failed: " << res
                  << " size=" << desc_.size << " name='" << (desc_.name ? desc_.name : "") << "'" << std::endl;
        return false;
    }

    // Persistent-mapped buffers:VMA 已经映射好了,缓存指针
    if (allocFlags & VMA_ALLOCATION_CREATE_MAPPED_BIT) {
        persistentMappedPtr_ = allocInfo.pMappedData;
        // 显式清零(避免 GPU 在 producer 写之前读到脏数据,与 MetalBuffer 行为一致)
        if (persistentMappedPtr_) {
            std::memset(persistentMappedPtr_, 0, desc_.size);
        }
    }

    // Debug label
    if (desc_.name && vkDevice.GetNativeDevice()) {
        VkDebugUtilsObjectNameInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        info.objectType = VK_OBJECT_TYPE_BUFFER;
        info.objectHandle = reinterpret_cast<u64>(vkBuffer_);
        info.pObjectName = desc_.name;
        // Best-effort:函数指针可能未加载,失败不影响功能
        if (auto* setName = vkDevice.GetDebugUtilsSetObjectName()) {
            setName(vkDevice.GetNativeDevice(), &info);
        }
    }

    state_ = ResourceState::Ready;
    return true;
}

void VulkanBuffer::destroyImpl() {
    if (vkBuffer_ != VK_NULL_HANDLE || allocation_ != nullptr) {
        VkBuffer buffer = vkBuffer_;
        VmaAllocation alloc = allocation_;
        VmaAllocator allocator = static_cast<VulkanDevice&>(device_).GetVmaAllocator();

        device_.GetGarbageCollector().DeferredDestroy([allocator, buffer, alloc]() {
            if (allocator && buffer != VK_NULL_HANDLE && alloc) {
                vmaDestroyBuffer(allocator, buffer, alloc);
            }
        });
    }

    vkBuffer_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    persistentMappedPtr_ = nullptr;
}

void* VulkanBuffer::mapImpl(u64 offset, u64 /*size*/) {
    // Persistent-mapped 路径(零成本):Dynamic/Staging/Readback 走这里
    if (persistentMappedPtr_) {
        return static_cast<u8*>(persistentMappedPtr_) + offset;
    }

    // 非 persistent 路径(理论上只在静态缓冲区显式 map 时出现 — 通常被 CanMap() 拒绝)
    if (vkBuffer_ == VK_NULL_HANDLE || !allocation_) return nullptr;

    VulkanDevice& vkDevice = static_cast<VulkanDevice&>(device_);
    VmaAllocator allocator = vkDevice.GetVmaAllocator();
    void* ptr = nullptr;
    VkResult res = vmaMapMemory(allocator, allocation_, &ptr);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanBuffer] vmaMapMemory failed: " << res << std::endl;
        return nullptr;
    }
    return static_cast<u8*>(ptr) + offset;
}

void VulkanBuffer::unmapImpl() {
    // Persistent-mapped 缓冲区永远不应该 unmap(VMA 拥有映射)
    if (persistentMappedPtr_) return;
    if (!allocation_) return;

    VulkanDevice& vkDevice = static_cast<VulkanDevice&>(device_);
    VmaAllocator allocator = vkDevice.GetVmaAllocator();
    vmaUnmapMemory(allocator, allocation_);
}

bool VulkanBuffer::updateDataImpl(const void* data, u64 size, u64 offset) {
    if (!data || size == 0) return false;
    if (offset + size > desc_.size) return false;

    // Fast path:persistent-mapped(Dynamic/Staging/Readback)
    if (persistentMappedPtr_) {
        std::memcpy(static_cast<u8*>(persistentMappedPtr_) + offset, data, size);
        return true;
    }

    // Slow path:Static/Immutable 走 staging blit
    // 暂时占位 — Phase 3 接入 VulkanStagingAllocator + VulkanCommandBuffer 后填充
    // 当前先返回 false,让调用方知道这条路未实现
    std::cerr << "[VulkanBuffer] updateDataImpl slow path (staging blit) not yet wired — "
              << "size=" << size << " offset=" << offset << " name='"
              << (desc_.name ? desc_.name : "") << "'" << std::endl;
    return false;
}

} // namespace primal::graphics::rhi
