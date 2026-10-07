#include <metal_stdlib>
using namespace metal;

// Visibility Buffer Shader
// Renders geometry to a visibility buffer storing cluster + primitive IDs
// This is the FIRST PASS of GPU-driven rendering
//
// [PassDebugView 排查 2026-09-24] 旧版 VS 是假实现：无视 compact_cluster_ids/
// cluster_map/instance_data（instance_id 直接当 meshlet_id、顶点裸拉 positions),
// 产物是乱三角形 + 错 ID。现镜像 GPUDrivenDraw.metal 的 gpu_driven_vertex_shader
// 正确管线（compact list → cluster_map → meshlet 顶点拉取 + 实例变换），
// 使 visibility/meshlet/cluster/mesh 四个 pass 视图拿到真实 ID。
// 注意 buffer(4) 必须是 packed_float3（全局 position 流是 12B 紧凑布局）。

struct MeshletData {
    uint vertex_offset;      // u32 单位（meshlet_vertices 数组内偏移）
    uint triangle_offset;    // 字节单位（u8 数组内偏移）
    uint vertex_count;
    uint triangle_count;

    // 与 C++ rhi::RHIMeshlet 对齐（64 字节总量）
    packed_float3 cone_apex;
    packed_float3 cone_axis;
    float cone_cutoff;
    packed_float3 center;
    float radius;
    uint padding;
};

struct ClusterMap {
    uint globalMeshletIndex;
    uint instanceIndex;
    uint materialID;
    uint padding;
};

struct InstanceData {
    float4x4 world_matrix;              // 64 bytes - offsets 0-63
    float4x4 inverse_world_matrix;      // 64 bytes - offsets 64-127
    uint geometry_id;                   // offset 128
    uint material_id;                   // offset 132
    uint cluster_start;                 // offset 136
    uint cluster_count;                 // offset 140
    uint cluster_map_base;              // offset 144
    uint padding;
    uint padding1;
    uint padding2;
    float3 bounds_center;               // offsets 160-171
    float bounds_radius;                // offset 172
    uint bounds_padding[4];             // total 192 bytes
};

struct VertexOut {
    float4 position [[position]];
    uint meshlet_id [[flat]];
    uint primitive_id [[flat]];
};

struct DrawConstants {
    float4x4 view_matrix;
    float4x4 proj_matrix;
    float4x4 world_matrix;
    uint view_width;
    uint view_height;
    uint meshlet_count;
    uint padding;
};

// Vertex shader for visibility buffer generation
vertex VertexOut visibility_vertex_shader(
    const device packed_float3* positions [[buffer(4)]],    // Global position data (12B stride)
    const device uint* meshlet_vertices [[buffer(2)]],      // Meshlet vertices (global vertex indices)
    device const uchar* meshlet_triangles [[buffer(3)]],    // Local vertex indices (uchar)
    constant DrawConstants& constants [[buffer(0)]],        // Camera constants
    constant MeshletData* meshlets [[buffer(1)]],           // Meshlet data
    const device uint* compact_cluster_ids [[buffer(5)]],   // Visible cluster IDs
    constant ClusterMap* cluster_map [[buffer(6)]],         // Cluster mapping
    constant InstanceData* instance_data [[buffer(7)]],     // Per-instance transform data
    uint vid [[vertex_id]],
    uint instance_id [[instance_id]])
{
    VertexOut out;

    // 1. Global cluster id from the compacted visible list
    uint globalClusterID = compact_cluster_ids[instance_id];

    // 2. Map to global meshlet + instance
    ClusterMap map = cluster_map[globalClusterID];
    uint globalMeshletIndex = map.globalMeshletIndex;
    uint instanceIndex = map.instanceIndex;

    MeshletData meshlet = meshlets[globalMeshletIndex];
    InstanceData instance = instance_data[instanceIndex];

    // 3. Meshlet vertex pulling（与 gpu_driven_vertex_shader 相同）
    uint actualTriangles = meshlet.triangle_count;
    uint actualVertexCount = actualTriangles * 3;

    if (vid >= actualVertexCount) {
        // Padding vertices → degenerate（clip 外）
        out.position = float4(2.0, 2.0, 2.0, 1.0);
        out.meshlet_id = globalMeshletIndex;
        out.primitive_id = 0;
        return out;
    }

    uchar localVertIdx = meshlet_triangles[meshlet.triangle_offset + vid];
    uint vertIdx = meshlet_vertices[meshlet.vertex_offset + localVertIdx];

    float3 world_pos = float3(positions[vertIdx]);
    float4 clip_pos = constants.proj_matrix * constants.view_matrix
                    * instance.world_matrix * float4(world_pos, 1.0);

    out.position = clip_pos;
    out.meshlet_id = globalMeshletIndex;             // 与 cluster map 反查键一致
    out.primitive_id = vid / 3;                      // meshlet 内本地三角形序号
    return out;
}

// Fragment shader for visibility buffer generation
fragment uint visibility_fragment_shader(
    VertexOut in [[stage_in]],
    constant DrawConstants& constants [[buffer(0)]])
{
    // Pack meshlet_id and primitive_id into single uint
    // High 8 bits: meshlet_id (255 meshlets max), Low 24 bits: primitive_id (16M primitives max)
    // PassDebugView.metal 的 cluster 反查即用 globalMeshletIndex 作键。
    uint meshlet_part = (in.meshlet_id & 0xFF) << 24;
    uint primitive_part = in.primitive_id & 0x00FFFFFF;
    uint visibility_value = meshlet_part | primitive_part;

    return visibility_value;
}
