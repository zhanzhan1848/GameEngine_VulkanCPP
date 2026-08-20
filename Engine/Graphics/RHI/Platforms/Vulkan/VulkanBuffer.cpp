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

// BufferType + bindFlags → VkBufferUsageFlags 转换(独立函数,供构造和 Initialize 复用)
//
// 引擎很多 caller 设置 desc.bindFlags 包含 ResourceUsage bits(IndirectArg/
// ConstantBuffer/UnorderedAccess/...)和 BufferUsageFlags bits(Indirect/Uniform/
// Storage/Vertex/Index/TransferSrc|Dst),但只有 desc.type 进入 BufferTypeToVkUsage
// 时被翻译。结果:
//   - GPUCullingPipeline indirect_args_buffer (bindFlags=IndirectArg|TransferDst,
//     type=Structured):缺 INDIRECT_BUFFER_BIT
//   - GPUDrivenDrawPipeline indirect_draw_buffer (bindFlags=Indirect|Storage,
//     type=Unknown):只剩 TRANSFER_SRC|DST,验证错误 "descriptorType STORAGE_BUFFER
//     but only TRANSFER flags"
//
// 这里把 desc.bindFlags 的两套位都翻译到 Vulkan 等价 bit。
VkBufferUsageFlags BufferDescToVkUsage(BufferType type, u32 bindFlags) {
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

    // ResourceUsage bits (engine-wide convention; matches RHIResource.h enum)
    if (bindFlags & static_cast<u32>(ResourceUsage::ShaderResource))  usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (bindFlags & static_cast<u32>(ResourceUsage::UnorderedAccess)) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (bindFlags & static_cast<u32>(ResourceUsage::CopySource))      usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (bindFlags & static_cast<u32>(ResourceUsage::CopyDest))        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (bindFlags & static_cast<u32>(ResourceUsage::IndexBuffer))     usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (bindFlags & static_cast<u32>(ResourceUsage::VertexBuffer))    usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (bindFlags & static_cast<u32>(ResourceUsage::ConstantBuffer))  usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (bindFlags & static_cast<u32>(ResourceUsage::IndirectArg))     usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    // BufferUsageFlags bits (RHI-side fine-grained flags; mirrors VkBufferUsageFlagBits)
    if (bindFlags & static_cast<u32>(BufferUsageFlags::TransferSrc)) usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (bindFlags & static_cast<u32>(BufferUsageFlags::TransferDst)) usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (bindFlags & static_cast<u32>(BufferUsageFlags::Uniform))     usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (bindFlags & static_cast<u32>(BufferUsageFlags::Storage))     usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (bindFlags & static_cast<u32>(BufferUsageFlags::Index))       usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (bindFlags & static_cast<u32>(BufferUsageFlags::Vertex))      usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (bindFlags & static_cast<u32>(BufferUsageFlags::Indirect))    usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    return usage;
}

} // anonymous namespace

VulkanBuffer::VulkanBuffer(VulkanDevice& device, const BufferDesc& desc)
    : RHIResource(device, ResourceDesc(
          ResourceType::Buffer,
          GetResourceUsageFromBufferType(desc.type, desc.bindFlags),
          // Engine callers split between `desc.usage` (legacy/primary) and
          // `desc.memoryUsage` (compat field). Pick whichever is set so
          // neither convention silently falls through to GPU-only allocation.
          (desc.memoryUsage != GPUMemoryUsage::Unknown) ? desc.memoryUsage : desc.usage,
          desc.size,
          desc.name.c_str())),
      vkUsageFlags_(BufferDescToVkUsage(desc.type, desc.bindFlags))
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

    // T4.6.5 part 30.11 (Bug H): slow path — Static/Immutable buffers are
    // DEVICE_LOCAL (no CPU mapping). Stage through a transient HOST_VISIBLE
    // buffer + vkCmdCopyBuffer on the graphics queue. This is called once per
    // Sponza mesh at init (~400 calls) and silently failing here was the root
    // cause of pure-black output: mesh vertex/index buffers stayed zeroed,
    // Stage2/Stage3 rasterized degenerate triangles, GBuffer stayed at clear.
    VulkanDevice& vkDev = static_cast<VulkanDevice&>(device_);
    VmaAllocator allocator = vkDev.GetVmaAllocator();
    if (allocator == VK_NULL_HANDLE || vkBuffer_ == VK_NULL_HANDLE) return false;

    // 1. Create + fill staging buffer (HOST_VISIBLE, persistent-mapped).
    VkBufferCreateInfo stagingCI{};
    stagingCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingCI.size = size;
    stagingCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingACI{};
    stagingACI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                     | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    stagingACI.usage = VMA_MEMORY_USAGE_AUTO;

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    VmaAllocationInfo stagingInfo{};
    if (vmaCreateBuffer(allocator, &stagingCI, &stagingACI,
                        &stagingBuf, &stagingAlloc, &stagingInfo) != VK_SUCCESS) {
        std::cerr << "[VulkanBuffer] updateDataImpl: staging vmaCreateBuffer failed"
                  << " size=" << size << std::endl;
        return false;
    }
    std::memcpy(stagingInfo.pMappedData, data, size);

    // 2. One-time-use command buffer on the graphics queue.
    // T4.6.5 part 30.11: each VulkanCommandBuffer owns its own VkCommandPool;
    // there is no device-wide accessor. Create a transient pool here, use
    // once, and tear down. Transient pool flag lets the driver recycle.
    VkDevice vkDevice = vkDev.GetNativeDevice();
    u32 queueFamily = vkDev.GetGraphicsQueueFamily();
    if (queueFamily == UINT32_MAX) {
        vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
        return false;
    }
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = queueFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &pci, nullptr, &pool) != VK_SUCCESS) {
        vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    allocInfo.commandPool = pool;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
        return false;
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = offset;
    region.size = size;
    vkCmdCopyBuffer(cmd, stagingBuf, vkBuffer_, 1, &region);

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
                          | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vkBuffer_;
    barrier.offset = offset;
    barrier.size = size;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);

    vkEndCommandBuffer(cmd);

    // 3. Submit + wait (synchronous — caller expects data ready on return).
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vkDev.GetGraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vkDev.GetGraphicsQueue());

    vkFreeCommandBuffers(vkDevice, pool, 1, &cmd);
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
    return true;
}

} // namespace primal::graphics::rhi
