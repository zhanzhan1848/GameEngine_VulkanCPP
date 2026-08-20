#version 450 core
// VoxelDebug.frag — hand-written GLSL port (voxel_debug_fs).
// Nearest-neighbor density re-sample + discard below threshold. Binding 3
// carries the nearest sampler (binding 2 is the linear one used by the
// vertex stage).

layout(set = 0, binding = 0) uniform texture3D voxelTexture;
layout(set = 0, binding = 3) uniform sampler nearestSampler;

layout(std140, set = 0, binding = 1) uniform DebugUniforms {
    mat4 viewProjection;
    mat4 model;
    float scale;
    float threshold;
    float step_throttle;
    float _pad0;
    uvec4 resolution;
};

layout(location = 0) in vec3 vUVW;
layout(location = 1) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    float density = texture(sampler3D(voxelTexture, nearestSampler), vUVW).r;
    if (density < threshold) {
        discard;
    }
    outColor = vec4(vColor, 1.0);
}
