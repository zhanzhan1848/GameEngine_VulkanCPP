/**
 * @file MetalStagingAllocator.h
 * @brief Per-frame ring-pooled staging allocator for Metal
 * @details Eliminates waitUntilCompleted from MetalBuffer/MetalTexture slow paths
 *          by allocating staging memory from a 4-frame ring pool and encoding
 *          blits into the active frame's command buffer.
 *
 * Lifecycle:
 *   - 4 frame pools (MAX_FRAMES_IN_FLIGHT=3 + 1 safety margin), each ~16MB
 *   - Each frame: BeginFrame() rotates to next pool and resets its bump offset
 *   - Allocate(size) returns CPU-writable pointer from current frame's pool
 *   - QueueBlit_*() records pending blit into current frame's pending list
 *   - EncodePendingBlits(cmd) encodes all queued blits as a single blit encoder
 *     at the start of the frame's command buffer, then clears the queue
 *
 * Synchronization:
 *   - RenderSystem::BeginFrame waits on per-frame SharedEvent for frame N-3
 *   - After that wait, frame N-3's staging memory is GPU-safe to reclaim
 *   - We rotate to the next pool which was last used 4 frames ago — safe to bump
 *
 * Out-of-frame (asset load) callers:
 *   - FlushBlocking() helper allocates staging, queues blit, creates a one-shot
 *     command buffer on the transfer queue, commits and waits
 *   - This preserves legacy semantics for callers without frame context
 */

#pragma once

#include "MetalCommon.h"
#include "../../Core/RHITypes.h"
#include <vector>

namespace primal::graphics::rhi {

class MetalStagingAllocator {
public:
    static constexpr u32 FRAME_COUNT = MAX_FRAMES_IN_FLIGHT + 1; // 4
    static constexpr u64 DEFAULT_POOL_SIZE = 16ULL * 1024ULL * 1024ULL; // 16MB per frame

    struct Allocation {
        void*        cpuPtr        = nullptr;  // CPU-writable mapped pointer
        MTL::Buffer* stagingBuffer = nullptr;  // Pool buffer containing this allocation
        u64          offset        = 0;        // Byte offset within stagingBuffer
        bool         overflow      = false;    // True if pool was exhausted (caller must handle)
    };

    MetalStagingAllocator() = default;
    ~MetalStagingAllocator();

    MetalStagingAllocator(const MetalStagingAllocator&) = delete;
    MetalStagingAllocator& operator=(const MetalStagingAllocator&) = delete;

    /**
     * @brief Initialize the per-frame ring pools
     * @param device Metal device (retained)
     * @param transferQueue Transfer queue (used by FlushBlocking; not retained — caller owns)
     * @param poolSize Per-frame pool size in bytes (default 16MB)
     */
    void Initialize(MTL::Device* device, MTL::CommandQueue* transferQueue,
                    u64 poolSize = DEFAULT_POOL_SIZE);

    /**
     * @brief Release all per-frame pools
     */
    void Shutdown();

    /**
     * @brief Begin a new frame — rotate to next pool and reset its bump offset
     * @details Must be called AFTER the per-frame fence wait completes (i.e.,
     *          frame N-FRAME_COUNT's command buffer is GPU-complete).
     *          Pending blit queue for the new frame is cleared.
     */
    void BeginFrame();

    /**
     * @brief Allocate CPU-writable staging memory from the current frame's pool
     * @param size Bytes to allocate
     * @param alignment Byte alignment (default 256 — Metal minimum for buffer-to-texture blit)
     * @return Allocation with cpuPtr + buffer + offset. If pool exhausted, Allocation.overflow=true.
     *
     * Caller writes data to cpuPtr, then calls QueueBlit_* to record the GPU copy.
     */
    Allocation Allocate(u64 size, u64 alignment = 256);

    /**
     * @brief Queue a buffer-to-buffer blit to be encoded later via EncodePendingBlits
     */
    void QueueBlit_Buffer(Allocation alloc,
                          MTL::Buffer* dstBuffer, u64 dstOffset, u64 size);

    /**
     * @brief Queue a buffer-to-texture blit to be encoded later via EncodePendingBlits
     * @param bytesPerRow Source layout — caller computes via bytesPerComponent * width
     * @param bytesPerImage Source layout — bytesPerRow * height for 2D, or per slice for 3D
     */
    void QueueBlit_Texture(Allocation alloc,
                           MTL::Texture* dstTexture, u32 mipLevel, u32 slice,
                           u32 originX, u32 originY, u32 originZ,
                           u32 width, u32 height, u32 depth,
                           u32 bytesPerRow, u32 bytesPerImage);

    /**
     * @brief Encode all pending blits into the given command buffer
     * @details Creates a single MTLBlitCommandEncoder on cmd, encodes all queued
     *          blits for the current frame, ends encoding, and clears the queue.
     *          Must be called BEFORE any user render/compute passes on cmd so
     *          that staging uploads are visible to subsequent passes.
     */
    void EncodePendingBlits(MTL::CommandBuffer* cmd);

    /**
     * @brief Blocking flush — used when no per-frame command buffer is available
     * @details Allocates staging, queues blit, creates a one-shot transfer cmdbuf,
     *          commits, and waits for completion. Preserves legacy asset-load
     *          semantics. Caller passes allocation/blit params via the same APIs
     *          (Allocate + QueueBlit_*) before calling this.
     */
    void FlushBlocking();

    /**
     * @brief Whether the allocator has been initialized
     */
    bool IsInitialized() const { return device_ != nullptr; }

private:
    struct PendingBlit {
        // Source
        MTL::Buffer*  srcBuffer  = nullptr;
        u64           srcOffset  = 0;

        // Destination (one of)
        MTL::Buffer*  dstBuffer  = nullptr;
        MTL::Texture* dstTexture = nullptr;

        // Buffer blit params
        u64           dstBufferOffset = 0;

        // Texture blit params
        u32           mipLevel        = 0;
        u32           slice           = 0;
        u32           originX         = 0;
        u32           originY         = 0;
        u32           originZ         = 0;
        u32           width           = 0;
        u32           height          = 0;
        u32           depth           = 0;
        u32           bytesPerRow     = 0;
        u32           bytesPerImage   = 0;

        u64           size            = 0;  // For buffer blits
        bool          isTexture       = false;
    };

    struct FramePool {
        MTL::Buffer*              buffer  = nullptr;  // Shared-mode pool
        u64                       offset  = 0;        // Current bump offset
        u64                       capacity = 0;
        std::vector<PendingBlit>  pending;            // Per-frame blit queue
    };

    MTL::Device*         device_           = nullptr;  // Retained
    MTL::CommandQueue*   transferQueue_    = nullptr;  // Not retained (caller owns)
    FramePool            pools_[FRAME_COUNT];
    u32                  currentFrame_     = 0;
    bool                 initialized_      = false;
};

} // namespace primal::graphics::rhi
