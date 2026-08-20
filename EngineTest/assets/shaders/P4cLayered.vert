#version 460
// P4c-F5 layered rendering: fullscreen triangle, one instance per array layer.
// gl_Layer output selects the destination layer (core in Vulkan 1.2 / MoltenVK;
// Metal equivalent = renderTargetArrayLength + [[render_target_array_index]]).
// Single-layer mode: push constant layerId drives content, gl_Layer stays 0.
#extension GL_ARB_shader_viewport_layer_array : require

layout(push_constant) uniform PC {
    int layerId;
    int useGlLayer;
} pc;

out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out flat int vLayer;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    if (pc.useGlLayer != 0) {
        gl_Layer = int(gl_InstanceIndex);
        vLayer = int(gl_InstanceIndex);
    } else {
        // 单层模式不写 gl_Layer(1-layer framebuffer 上的写出会触发
        // VUID 警告且值未定义)。
        vLayer = pc.layerId;
    }
}
