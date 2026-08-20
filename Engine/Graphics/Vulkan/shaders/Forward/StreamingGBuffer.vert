#version 450 core

// T4.6.5 part 15 — Vulkan port of Forward/StreamingGBuffer.metal vertex stage.
// Entry point: main
//
// SoA (Structure-of-Arrays) vertex pulling variant of GBuffer.vert.
// StreamingMesh stores positions, elements, and indices as separate GPU buffers
// instead of the interleaved AoS VertexInput used by GBuffer.metal.
//
// Metal [[buffer(N)]] → Vulkan binding map:
//   buffer(0)  ViewData    → set 0 binding 0 (UBO)
//   buffer(1)  SceneData   → set 0 binding 1 (UBO)
//   buffer(2)  PushConsts  → push_constant
//   buffer(3)  InstanceData → (unused for streaming; use_instances=0)
//   buffer(20) positions   → set 0 binding 3 (SSBO, readonly) — Vulkan-only addition
//   buffer(21) elements    → set 0 binding 4 (SSBO, readonly) — Vulkan-only addition
//   buffer(22) indices     → set 0 binding 5 (SSBO, readonly) — Vulkan-only addition
//
// Layout strategy: Option B (extend global_set_layout_ on Vulkan). The Metal
// path uses BindVertexBuffers(20,3,...) + per-vertex [[buffer(N)]] semantics
// that allow manual indirection `positions[indices[vertexId]]` in shader.
// Vulkan cannot do this via vkCmdBindVertexBuffers alone (per-vertex attribute
// only yields attributes[gl_VertexIndex], not attributes[indices[gl_VertexIndex]]).
// Therefore we bind all three SoA buffers as SSBOs and replicate the Metal
// indirection logic explicitly. This requires extending global_set_layout_ with
// bindings 3/4/5 on Vulkan (see ForwardSceneRenderer::CreateDescriptorLayouts).
//
// Elements struct (20B stride, packed — matches content::PackVertexElement):
//   offset  0: u32   ColorTSign
//   offset  4: u16[2] Normal   (packed_ushort2)
//   offset  8: u16[2] Tangent  (packed_ushort2)
//   offset 12: f32[2] UV
// GLSL std430 SSBO of struct must use 16B alignment for uvec2/vec2 members;
// we declare the struct with explicit packing and read raw u16/f32 via bitcast.
//
// Push constants (PCGPushConsts, 80B): mat4 transform; vec4 _use_pad (.x=use_instances)

#define SET_GLOBAL 0

layout(set = SET_GLOBAL, binding = 0) uniform ViewData {
    mat4 viewProjection;
    mat4 invViewProjection;
    mat4 previousViewProjection;
} viewData;

layout(set = SET_GLOBAL, binding = 1) uniform SceneData {
    mat4 model;
    vec4 lightPos;
    vec4 lightColor;
    vec4 reflectionPlane;
    vec4 reflectionPlane2;
    vec4 reflectionPlane3;
    mat4 previousModel;
    vec2 jitter;
    vec2 previousJitter;
    vec2 padding;
    vec4 viewPos;
    mat4 shadowMatrix0;
    mat4 shadowMatrix1;
} sceneData;

// T4.6.5 part 11: std140 layout trap. Pack (use_instances + _pad[3]) into a
// single vec4 to keep push constants at exactly 80B (mat4 + vec4).
layout(push_constant) uniform PushConsts {
    mat4 transform;
    vec4 _use_pad;  // .x = use_instances, .yzw = _pad[0..2]
} pc;
#define use_instances _use_pad.x

// --- SoA vertex pulling buffers (Option B: Vulkan-only SSBO bindings 3/4/5) ---
// positions: packed_float3 (12B stride). GLSL std430 f32 array; we read 3
// consecutive floats and reconstruct the vec3.
layout(set = SET_GLOBAL, binding = 3) readonly buffer PositionsBuf {
    float positions[];  // 3 floats per vertex, no padding
} posBuf;

// elements: 20B stride packed body. Read as raw u32 stream and bitcast
// members out (matches the C++ content::PackVertexElement layout exactly).
//   elem_idx_base = vid * 5u;  // 20B / 4B = 5 u32s per element
//   colorTSign = u32 at +0
//   normal.xy  = u16x2 at +4   (read as lo/hi halves of u32 at +4)
//   tangent.xy = u16x2 at +8
//   uv         = f32x2 at +12  (= 2 u32s starting at +12, = u32[3..4])
layout(set = SET_GLOBAL, binding = 4) readonly buffer ElementsBuf {
    uint elements[];  // 5 u32s per element (20B stride)
} elemBuf;

// indices: u32 stride. Manual indexed drawing — gl_VertexIndex is the draw
// index (0..vertexCount-1); indices[gl_VertexIndex] is the actual vertex id.
layout(set = SET_GLOBAL, binding = 5) readonly buffer IndicesBuf {
    uint indices[];
} idxBuf;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec3 outWorldTangent;
layout(location = 3) out vec2 outUV;
layout(location = 4) out vec4 outCurrentPos;
layout(location = 5) out vec4 outPreviousPos;

const float InvIntervals = 2.0 / ((1 << 16) - 1);

vec3 UnpackNormal(uvec2 p) {
    vec2 f = vec2(p);
    f = f * InvIntervals - 1.0;
    float d = dot(f, f);
    if (d > 1.0) {
        return vec3(0.0, 0.0, 1.0);  // invalid data fallback
    }
    float z = sqrt(max(0.0, 1.0 - d));
    return vec3(f.x, f.y, z);
}

void main() {
    // Manual indexed drawing: gl_VertexIndex is the draw index, indices[] maps
    // each invocation to the actual vertex into positions/elements.
    uint vid = idxBuf.indices[gl_VertexIndex];

    // StreamingMesh positions are already in world space; pc.transform is
    // identity in the streaming path. use_instances path is not wired for
    // streaming (defaults to 0).
    mat4 model = pc.transform;

    // Read packed position (3 floats, 12B stride).
    vec3 rawPos = vec3(posBuf.positions[vid * 3u + 0u],
                       posBuf.positions[vid * 3u + 1u],
                       posBuf.positions[vid * 3u + 2u]);

    // Read element fields from packed 20B body (5 u32s).
    uint elemBase = vid * 5u;
    // Normal: packed_ushort2 at byte offset 4 → low 16 bits of u32[1].
    uint normalWord  = elemBuf.elements[elemBase + 1u];
    uvec2 rawNormalPacked = uvec2(normalWord & 0xFFFFu, normalWord >> 16u);
    // Tangent: packed_ushort2 at byte offset 8 → low 16 bits of u32[2].
    uint tangentWord = elemBuf.elements[elemBase + 2u];
    uvec2 rawTangentPacked = uvec2(tangentWord & 0xFFFFu, tangentWord >> 16u);
    // UV: packed_float2 at byte offset 12 → u32[3..4].
    uint uvWord0 = elemBuf.elements[elemBase + 3u];
    uint uvWord1 = elemBuf.elements[elemBase + 4u];
    vec2 rawUV = vec2(uintBitsToFloat(uvWord0), uintBitsToFloat(uvWord1));

    vec4 worldPos = model * vec4(rawPos, 1.0);
    outWorldPos = worldPos.xyz;

    mat3 normalMatrix = mat3(model[0].xyz, model[1].xyz, model[2].xyz);

    vec3 rawNormal = UnpackNormal(rawNormalPacked);
    outWorldNormal = normalize(normalMatrix * rawNormal);

    vec3 rawTangent = UnpackNormal(rawTangentPacked);
    outWorldTangent = normalize(normalMatrix * rawTangent);

    // V-flip UV to match Metal convention (matches GBuffer.vert).
    outUV = vec2(rawUV.x, 1.0 - rawUV.y);

    gl_Position = viewData.viewProjection * worldPos;

    outCurrentPos = gl_Position;
    outPreviousPos = viewData.previousViewProjection * (sceneData.previousModel * vec4(rawPos, 1.0));
}
