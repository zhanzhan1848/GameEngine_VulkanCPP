#include "Common.h"

struct ParticleData {
    float4 position;      // xyz = position, w = age (0-1)
    float4 velocity;      // xyz = velocity, w = lifetime
    float4 color;         // rgba
    float4 scale_rotation; // xy = scale, zw = rotation
};

struct ParticleUniforms {
    float4x4 view_projection;
    float4x4 view_matrix;
    float time;
    float delta_time;
    uint vertex_count;
    uint blend_mode;
    uint texture_frames_x;
    uint texture_frames_y;
    float frame_rate;
};

struct VisibleIndices {
    device uint* visible_indices [[id(0)]];
};

struct ParticleBuffer {
    device ParticleData* particles [[id(0)]];
};

struct ParticleVertexOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float age;
    uint texture_frame;
};

struct QuadVertex {
    float2 position;
    float2 uv;
};

constant QuadVertex quad_vertices[6] = {
    { float2(-0.5, -0.5), float2(0.0, 0.0) },
    { float2( 0.5, -0.5), float2(1.0, 0.0) },
    { float2(-0.5,  0.5), float2(0.0, 1.0) },
    { float2(-0.5,  0.5), float2(0.0, 1.0) },
    { float2( 0.5, -0.5), float2(1.0, 0.0) },
    { float2( 0.5,  0.5), float2(1.0, 1.0) }
};

float2x2 rotate2d(float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return float2x2(c, -s, s, c);
}

vertex ParticleVertexOut particle_vertex_instanced(
    device const ParticleUniforms& uniforms [[buffer(0)]],
    device const ParticleBuffer& particle_buffer [[buffer(1)]],
    device const VisibleIndices& visible_indices [[buffer(2)]],
    uint vertex_id [[vertex_id]],
    uint instance_id [[instance_id]])
{
    ParticleVertexOut out;
    
    uint particle_idx = visible_indices.visible_indices[instance_id];
    ParticleData p = particle_buffer.particles[particle_idx];
    
    QuadVertex quad = quad_vertices[vertex_id];
    out.uv = quad.uv;
    
    float2 scale = p.scale_rotation.xy;
    float rotation = p.scale_rotation.z;
    
    // Extract camera vectors from view matrix for billboarding
    float3 camera_right = float3(uniforms.view_matrix[0][0], uniforms.view_matrix[1][0], uniforms.view_matrix[2][0]);
    float3 camera_up = float3(uniforms.view_matrix[0][1], uniforms.view_matrix[1][1], uniforms.view_matrix[2][1]);
    
    // Apply rotation to UV
    float2x2 rot = rotate2d(rotation);
    float2 rotated_uv = rot * (quad.uv - 0.5) + 0.5;
    out.uv = rotated_uv;
    
    // Calculate billboard position
    float3 world_pos = p.position.xyz;
    float3 offset = camera_right * quad.position.x * scale.x + camera_up * quad.position.y * scale.y;
    
    float4 world_position = float4(world_pos + offset, 1.0);
    out.position = uniforms.view_projection * world_position;
    
    // Color with age-based alpha fade
    float age = p.position.w;
    float alpha_fade = 1.0 - smoothstep(0.7, 1.0, age);
    out.color = float4(p.color.rgb, p.color.a * alpha_fade);
    out.age = age;
    out.texture_frame = 0;
    
    return out;
}

vertex ParticleVertexOut particle_vertex_point(
    device const ParticleUniforms& uniforms [[buffer(0)]],
    device const ParticleBuffer& particle_buffer [[buffer(1)]],
    device const VisibleIndices& visible_indices [[buffer(2)]],
    uint vertex_id [[vertex_id]])
{
    ParticleVertexOut out;
    
    uint particle_idx = visible_indices.visible_indices[vertex_id];
    ParticleData p = particle_buffer.particles[particle_idx];
    
    float2 scale = p.scale_rotation.xy;
    float rotation = p.scale_rotation.z;
    
    float3 world_pos = p.position.xyz;
    
    out.position = uniforms.view_projection * float4(world_pos, 1.0);
    
    // Set point size based on scale and distance
    float dist = length(world_pos);
    float point_size = (scale.x + scale.y) * 0.5 * 100.0 / max(dist, 0.1);
    // out.point_size = point_size;  // Uncomment if using point sprites
    
    float age = p.position.w;
    float alpha_fade = 1.0 - smoothstep(0.8, 1.0, age);
    out.color = float4(p.color.rgb, p.color.a * alpha_fade);
    out.uv = float2(0.5, 0.5);
    out.age = age;
    out.texture_frame = 0;
    
    return out;
}

fragment float4 particle_fragment(
    ParticleVertexOut in [[stage_in]],
    texture2d<float> particle_texture [[texture(0)]],
    sampler tex_sampler [[sampler(0)]])
{
    float4 tex_color = particle_texture.sample(tex_sampler, in.uv);
    float4 final_color = tex_color * in.color;
    
    if (final_color.a < 0.01) {
        discard_fragment();
    }
    
    return final_color;
}

fragment float4 particle_fragment_untextured(
    ParticleVertexOut in [[stage_in]])
{
    // Create circular shape from UV
    float2 centered = in.uv - 0.5;
    float dist = length(centered);
    
    if (dist > 0.5) {
        discard_fragment();
    }
    
    // Soft edge
    float alpha = 1.0 - smoothstep(0.3, 0.5, dist);
    
    return float4(in.color.rgb, in.color.a * alpha);
}
