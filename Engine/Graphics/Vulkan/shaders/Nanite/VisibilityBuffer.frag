#version 450 core

// T4.6.5 part 17.3 — Vulkan port of EngineTest/shaders/VisibilityBuffer.metal
// Fragment stage. Source fn is `main`; -e renames the SPIR-V entry point to
// `visibility_fragment_shader`.
// Output: R32_UINT packed (meshlet_id << 24 | primitive_id & 0x00FFFFFF).

layout(location = 0) in VSOut {
    vec4 position;
    float depth;
    flat uint meshlet_id;
    flat uint primitive_id;
} IN;

layout(location = 0) out uint out_visibility;

void main() {
    uint meshlet_part = (IN.meshlet_id & 0xFFu) << 24u;
    uint primitive_part = IN.primitive_id & 0x00FFFFFFu;
    out_visibility = meshlet_part | primitive_part;
}
