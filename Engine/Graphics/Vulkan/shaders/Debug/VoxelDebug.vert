#version 450 core
// VoxelDebug.vert — hand-written GLSL port of
// RHI/Shaders/Debug/VoxelDebug.metal (voxel_debug_vs).
// Instanced cube (36-vertex triangle list) per voxel; VS samples the density
// texture and collapses empty voxels.

layout(set = 0, binding = 0) uniform texture3D voxelTexture;
layout(set = 0, binding = 2) uniform sampler linearSampler;

layout(std140, set = 0, binding = 1) uniform DebugUniforms {
    mat4 viewProjection;       // offset 0
    mat4 model;                // offset 64
    float scale;               // offset 128 (union: voxel)
    float threshold;           // offset 132
    float step_throttle;       // offset 136 — 'step' is a GLSL builtin name
    float _pad0;               // offset 140
    uvec4 resolution;          // offset 144 — res[0..2] + pad
};

layout(location = 0) out vec3 vUVW;
layout(location = 1) out vec3 vColor;

const vec3 cubeTris[36] = vec3[36](
    // Front
    vec3(-0.5, -0.5,  0.5), vec3( 0.5, -0.5,  0.5), vec3( 0.5,  0.5,  0.5),
    vec3(-0.5, -0.5,  0.5), vec3( 0.5,  0.5,  0.5), vec3(-0.5,  0.5,  0.5),
    // Back
    vec3( 0.5, -0.5, -0.5), vec3(-0.5, -0.5, -0.5), vec3(-0.5,  0.5, -0.5),
    vec3( 0.5, -0.5, -0.5), vec3(-0.5,  0.5, -0.5), vec3( 0.5,  0.5, -0.5),
    // Top
    vec3(-0.5,  0.5,  0.5), vec3( 0.5,  0.5,  0.5), vec3( 0.5,  0.5, -0.5),
    vec3(-0.5,  0.5,  0.5), vec3( 0.5,  0.5, -0.5), vec3(-0.5,  0.5, -0.5),
    // Bottom
    vec3(-0.5, -0.5, -0.5), vec3( 0.5, -0.5, -0.5), vec3( 0.5, -0.5,  0.5),
    vec3(-0.5, -0.5, -0.5), vec3( 0.5, -0.5,  0.5), vec3(-0.5, -0.5,  0.5),
    // Right
    vec3( 0.5, -0.5,  0.5), vec3( 0.5, -0.5, -0.5), vec3( 0.5,  0.5, -0.5),
    vec3( 0.5, -0.5,  0.5), vec3( 0.5,  0.5, -0.5), vec3( 0.5,  0.5,  0.5),
    // Left
    vec3(-0.5, -0.5, -0.5), vec3(-0.5, -0.5,  0.5), vec3(-0.5,  0.5,  0.5),
    vec3(-0.5, -0.5, -0.5), vec3(-0.5,  0.5,  0.5), vec3(-0.5,  0.5, -0.5)
);

void main() {
    uvec3 res = resolution.xyz;
    if (res.x == 0u || res.y == 0u || res.z == 0u) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        vUVW = vec3(0.0);
        vColor = vec3(0.0);
        return;
    }

    uint actualID = gl_InstanceIndex * uint(step_throttle);

    uint z = actualID / (res.x * res.y);
    uint temp = actualID % (res.x * res.y);
    uint y = temp / res.x;
    uint x = temp % res.x;

    if (z >= res.z) {
        gl_Position = vec4(0.0);
        vUVW = vec3(0.0);
        vColor = vec3(0.0);
        return;
    }

    vec3 uvw = (vec3(x, y, z) + 0.5) / vec3(res);
    vUVW = uvw;

    float density = texture(sampler3D(voxelTexture, linearSampler), uvw).r;

    if (isnan(density) || isinf(density) || density < threshold) {
        gl_Position = vec4(0.0);  // collapse
        vColor = vec3(0.0);
        return;
    }

    // Model matrix maps volume space [-0.5, 0.5] to world bounds.
    vec3 voxelSize = 1.0 / vec3(res);
    vec3 vertPos = cubeTris[gl_VertexIndex];  // [-0.5, 0.5]
    vec3 center = (vec3(x, y, z) + 0.5) * voxelSize;
    vec3 pos = center + vertPos * voxelSize * scale;
    vec3 modelSpacePos = pos - 0.5;

    vec4 worldPos = model * vec4(modelSpacePos, 1.0);
    gl_Position = viewProjection * worldPos;

    vColor = vec3(density);
}
