#version 450 core
// MirrorReflection.vert — hand-written GLSL. Entry point: main
// (Metal: mirror_reflection_vs in EngineTest/shaders/MirrorReflection.metal).
//
// Renders the scene from a REFLECTED camera into the mirror's 1024² RT via
// vertex pulling — same meshlet conventions as ShadowDepth/GPUDrivenDraw but
// with a simplified forward-lit output (lambert + ambient, albedo from the
// material texture array). Drawn via DrawIndirect(378 verts, visibleCount).
//
// Bindings (flat, mirrors the C++ reflection_descriptor_layout_):
//   0 = UBO   ReflectionParams (VS|PS)
//   1 = SSBO  meshlets            (RHIMeshlet, 64B stride)
//   2 = SSBO  meshlet vertex indices (u32)
//   3 = SSBO  meshlet triangle indices (u8 packed in u32)
//   4 = SSBO  vertex positions    (packed_float3, 12B/vertex)
//   5 = SSBO  vertex elements     (6 u32/vertex: color_t_sign, normal, tangent, pad, uv.x, uv.y)
//   6 = SSBO  reflection visible entries (vec4 u32: meshletIdx, instanceIdx, materialId, pad)
//   7 = SSBO  instances           (InstanceData, 192B)
//   8 = SSBO  material data       (12 u32/material)

layout(std140, set = 0, binding = 0) uniform ReflectionParams {
    mat4 reflected_view_projection;  // offset 0
    vec4 light_dir;                  // offset 64, xyz = TO-light direction
    vec4 light_color;                // offset 80
    vec4 ambient;                    // offset 96
};

struct MeshletHeader {
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
    vec4 cone0;
    vec4 cone1;
    vec4 cone2;
};

layout(set = 0, binding = 1) readonly buffer MeshletBuffer   { MeshletHeader meshlets[]; };
layout(set = 0, binding = 2) readonly buffer VertexIndices   { uint meshletVertices[]; };
layout(set = 0, binding = 3) readonly buffer TriIndices      { uint meshletTrianglesPacked[]; };
layout(set = 0, binding = 4) readonly buffer Positions       { float positions[]; };
layout(set = 0, binding = 5) readonly buffer Elements        { uint elements[]; };
layout(set = 0, binding = 6) readonly buffer ReflVisible     { uvec4 reflVisible[]; };

struct InstanceData {
    mat4 world_matrix;
    mat4 inverse_world_matrix;
    uvec4 meta0;   // geometry_id, material_id, cluster_start, cluster_count
    uvec4 meta1;   // cluster_map_base, pad, pad, pad
    vec4 bounds_center_pad;  // xyz center, w pad
    vec4 bounds_radius_pad;  // x radius, yzw pad
};
layout(set = 0, binding = 7) readonly buffer InstanceBuffer { InstanceData instances[]; };

layout(set = 0, binding = 8) readonly buffer MaterialData { uint materialData[]; };

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vNormalW;
layout(location = 2) out vec3 vAlbedoTint;
layout(location = 3) flat out uint vAlbedoIdx;

uint readTriangleByte(uint byteIndex) {
    uint packed = meshletTrianglesPacked[byteIndex >> 2u];
    return (packed >> (8u * (byteIndex & 3u))) & 0xFFu;
}

// Octahedral-ish packed normal — 2×u16 in a u32, decoder per GPUDrivenDraw.wgsl
// unpack_normal (scale 2/65535; z sign from color_t_sign byte bit 1).
vec3 unpackNormal(uint packed, uint colorTSign) {
    float hi = float((packed >> 16u) & 0xFFFFu);
    float lo = float(packed & 0xFFFFu);
    vec2 f = vec2(hi, lo) * (2.0 / 65535.0) - vec2(1.0);
    float d = dot(f, f);
    if (d > 1.0) return vec3(0.0, 0.0, 1.0);
    float z = sqrt(max(0.0, 1.0 - d));
    uint signs = (colorTSign >> 24u) & 0xFFu;
    float nSign = float(signs & 0x02u) - 1.0;
    return vec3(f.x, f.y, z * nSign);
}

void main() {
    uint vertexID = gl_VertexIndex;
    uint listIdx = gl_InstanceIndex;

    uvec4 entry = reflVisible[listIdx];
    MeshletHeader meshlet = meshlets[entry.x];
    InstanceData inst = instances[entry.y];

    uint triIndex = vertexID;
    if (triIndex >= meshlet.triangle_count * 3u) {
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        vUV = vec2(0.0);
        vNormalW = vec3(0.0, 1.0, 0.0);
        vAlbedoTint = vec3(0.0);
        vAlbedoIdx = 0u;
        return;
    }

    uint localVertIdx = readTriangleByte(meshlet.triangle_offset + triIndex);
    uint vertIdx = meshletVertices[meshlet.vertex_offset + localVertIdx];

    vec3 pos = vec3(positions[3u * vertIdx + 0u],
                    positions[3u * vertIdx + 1u],
                    positions[3u * vertIdx + 2u]);

    // Element: 6 u32 per vertex — need normal (+sign) and uv.
    uint eBase = vertIdx * 6u;
    uint colorTSign = elements[eBase + 0u];
    uint packedNormal = elements[eBase + 1u];
    vec2 uv = vec2(uintBitsToFloat(elements[eBase + 4u]),
                   uintBitsToFloat(elements[eBase + 5u]));
    uv.y = 1.0 - uv.y;  // asset convention, same as GPUDrivenDraw.wgsl

    // Material: 12 u32 per entry — albedo texture index, tint, uv scale.
    uint mBase = entry.z * 12u;
    vAlbedoIdx = materialData[mBase + 0u];
    vAlbedoTint = vec3(uintBitsToFloat(materialData[mBase + 3u]),
                       uintBitsToFloat(materialData[mBase + 4u]),
                       uintBitsToFloat(materialData[mBase + 5u]));
    vec2 uvScale = vec2(uintBitsToFloat(materialData[mBase + 9u]),
                        uintBitsToFloat(materialData[mBase + 10u]));
    vUV = uv * uvScale;

    // World-space position + normal, then into the reflected clip space.
    vec4 worldPos = inst.world_matrix * vec4(pos, 1.0);
    gl_Position = reflected_view_projection * worldPos;

    vec3 n = unpackNormal(packedNormal, colorTSign);
    mat3 normalMatrix = mat3(inst.world_matrix[0].xyz,
                             inst.world_matrix[1].xyz,
                             inst.world_matrix[2].xyz);
    vNormalW = normalize(normalMatrix * n);
}
