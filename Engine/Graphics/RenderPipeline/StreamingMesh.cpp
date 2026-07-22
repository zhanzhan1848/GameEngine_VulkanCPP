#include "Graphics/RenderPipeline/StreamingMesh.h"

namespace primal::graphics {

namespace {
rhi::ResourceHandle make_storage_buf(rhi::RHIDeviceBase* device, u64 bytes) {
    rhi::BufferDesc desc{};
    desc.size = bytes;
    desc.bindFlags = (u32)rhi::BufferUsageFlags::Storage;
    // Dynamic (Shared storage on Metal) so MapBuffer works for the 8-byte
    // counter readback. Static would be Private -> contents()==nullptr.
    desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    desc.usage = rhi::GPUMemoryUsage::Dynamic;
    return device->CreateBuffer(desc);
}
} // namespace

StreamingMesh CreateStreamingMesh(
    rhi::RHIDeviceBase* device,
    u32 resolution,
    const math::v3& bounds_min,
    const math::v3& bounds_max)
{
    StreamingMesh sm{};
    if (device == nullptr || resolution < 2 || resolution > 256) return sm;

    const u32 n   = resolution + 1;
    const u32 n3  = n * n * n;
    const u32 res3 = resolution * resolution * resolution;
    const u32 max_verts  = n3;                 // (res+1)^3 grid vertices worst case
    const u32 max_indices = 18u * res3;        // 6 tris x 3 idx per grid vertex worst case

    sm.positions     = make_storage_buf(device, sizeof(f32) * 3 * max_verts);
    sm.elements      = make_storage_buf(device, 20u * max_verts);
    sm.indices       = make_storage_buf(device, sizeof(u32) * max_indices);
    sm.counters      = make_storage_buf(device, sizeof(u32) * 2);
    sm.indirect_args = make_storage_buf(device, 16);  // MTLDrawPrimitivesIndirectCommand = 4 x u32

    if (!sm.IsValid()) {
        // Partial alloc - caller will invoke DestroyStreamingMesh to clean up.
        return sm;
    }

    sm.max_verts   = max_verts;
    sm.max_indices = max_indices;
    sm.bounds_min  = bounds_min;
    sm.bounds_max  = bounds_max;
    return sm;
}

void DestroyStreamingMesh(rhi::RHIDeviceBase* device, StreamingMesh& sm) {
    if (device == nullptr) return;
    if (sm.positions      != rhi::handles::INVALID_RESOURCE) device->DestroyBuffer(sm.positions);
    if (sm.elements       != rhi::handles::INVALID_RESOURCE) device->DestroyBuffer(sm.elements);
    if (sm.indices        != rhi::handles::INVALID_RESOURCE) device->DestroyBuffer(sm.indices);
    if (sm.counters       != rhi::handles::INVALID_RESOURCE) device->DestroyBuffer(sm.counters);
    if (sm.indirect_args  != rhi::handles::INVALID_RESOURCE) device->DestroyBuffer(sm.indirect_args);
    sm = StreamingMesh{};
}

} // namespace primal::graphics
