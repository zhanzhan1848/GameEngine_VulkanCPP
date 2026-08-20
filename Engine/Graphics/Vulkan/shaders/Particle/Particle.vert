#version 450 core

// T4.6.5 part 16.3 — Vulkan port of Engine/Graphics/Metal/shaders/ParticleAtlas.metal
// Entry point: main (Vulkan convention; Metal uses particle_vertex_instanced).
//
// Descriptor set layout (matches ParticlePass::initialize at ParticlePass.cpp:84):
//   binding 0 = UniformBuffer (ParticleUniforms, vertex stage)
//   binding 1 = StorageBuffer (ParticleData[], vertex stage)
//   binding 2 = StorageBuffer (visible_count u32, vertex stage)
//   binding 3 = CombinedImageSampler (atlas texture, pixel stage — declared in .frag)
//
// NOTE: ParticlePushConstants (152B) exceeds Vulkan's default maxPushConstantsSize
// (128B), so we read uniforms from the UBO at binding 0 instead. Metal still uses
// push constants; the C++ pipeline-layout branch drops the range on Vulkan.

struct ParticleData {
    vec4 position;        // xyz = position, w = age
    vec4 velocity;        // xyz = velocity, w = lifetime
    vec4 color;           // rgba
    vec4 scale_rotation;  // xy = scale, zw = rotation
    vec4 uv_params;       // x = atlas_index, y = frame_progress, zw = padding
};

layout(set = 0, binding = 0) uniform ParticleUniforms {
    mat4 view_projection;
    mat4 view_matrix;
    float time;
    float delta_time;
    uint  blend_mode;
    uint  texture_frames_x;
    uint  texture_frames_y;
    float frame_rate;
} pc;

layout(set = 0, binding = 1) readonly buffer ParticleBuffer {
    ParticleData particles[];
} particleData;

layout(set = 0, binding = 2) readonly buffer VisibleCountBuffer {
    uint visible_count;
} visibleCount;

// Quad vertices for instanced rendering (billboard) — matches Metal `quad_positions[6]`.
vec2 quad_positions[6];
vec2 quad_uvs[6];

void initQuadTables() {
    quad_positions[0] = vec2(-0.5, -0.5);
    quad_positions[1] = vec2( 0.5, -0.5);
    quad_positions[2] = vec2(-0.5,  0.5);
    quad_positions[3] = vec2(-0.5,  0.5);
    quad_positions[4] = vec2( 0.5, -0.5);
    quad_positions[5] = vec2( 0.5,  0.5);

    quad_uvs[0] = vec2(0.0, 0.0);
    quad_uvs[1] = vec2(1.0, 0.0);
    quad_uvs[2] = vec2(0.0, 1.0);
    quad_uvs[3] = vec2(0.0, 1.0);
    quad_uvs[4] = vec2(1.0, 0.0);
    quad_uvs[5] = vec2(1.0, 1.0);
}

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUV;
layout(location = 2) out float outAge;

void main() {
    initQuadTables();
    uint vertex_id  = gl_VertexIndex;
    uint instance_id = gl_InstanceIndex;

    // Cull instances beyond visible count — match Metal early-out sentinel.
    if (instance_id >= visibleCount.visible_count) {
        gl_Position = vec4(0.0, 0.0, -1000.0, 1.0);
        outColor = vec4(0.0);
        outUV = vec2(0.0);
        outAge = 1.0;
        return;
    }

    ParticleData p = particleData.particles[instance_id];

    vec2 scale = p.scale_rotation.xy;
    vec2 quad_pos = quad_positions[vertex_id];
    vec2 quad_uv = quad_uvs[vertex_id];

    // Billboard: extract camera vectors from view matrix columns.
    vec3 camera_right = vec3(pc.view_matrix[0][0], pc.view_matrix[1][0], pc.view_matrix[2][0]);
    vec3 camera_up    = vec3(pc.view_matrix[0][1], pc.view_matrix[1][1], pc.view_matrix[2][1]);

    vec3 world_pos = p.position.xyz;
    vec3 offset = camera_right * quad_pos.x * scale.x + camera_up * quad_pos.y * scale.y;
    // NOTE: world_position intentionally unused — debug override below supersedes
    // it (matches Metal's debug-fullscreen-quad behavior verbatim).

    // DEBUG (preserved from Metal): full-screen quad. Y-flipped for Vulkan NDC.
    vec2 debug_offsets[6];
    debug_offsets[0] = vec2(-0.5, -0.5);
    debug_offsets[1] = vec2( 0.5, -0.5);
    debug_offsets[2] = vec2(-0.5,  0.5);
    debug_offsets[3] = vec2(-0.5,  0.5);
    debug_offsets[4] = vec2( 0.5, -0.5);
    debug_offsets[5] = vec2( 0.5,  0.5);
    gl_Position = vec4(debug_offsets[vertex_id] * 2.0, 1.0, 1.0);

    // Calculate texture UV from atlas index.
    float atlas_index = p.uv_params.x;
    uint cols = pc.texture_frames_x;
    uint rows = pc.texture_frames_y;

    if (cols > 1u || rows > 1u) {
        float tile_x = mod(atlas_index, float(cols));
        float tile_y = floor(atlas_index / float(cols));
        vec2 tile_size = 1.0 / vec2(float(cols), float(rows));
        vec2 tile_offset = tile_size * vec2(tile_x, tile_y);
        outUV = tile_offset + quad_uv * tile_size;
    } else {
        outUV = quad_uv;
    }

    // Color with age-based alpha fade.
    float age = p.position.w;
    float alpha_fade = 1.0 - smoothstep(0.7, 1.0, age);
    outColor = vec4(p.color.rgb, p.color.a * alpha_fade);
    outAge = age;
}
