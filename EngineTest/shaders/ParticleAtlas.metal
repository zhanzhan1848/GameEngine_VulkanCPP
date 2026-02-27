// ParticleAtlas.metal - Self-contained particle shader
// No external dependencies

#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

// Particle data structure - must match CPU side (80 bytes)
struct ParticleData {
    float4 position;       // xyz = position, w = age
    float4 velocity;       // xyz = velocity, w = lifetime
    float4 color;          // rgba
    float4 scale_rotation; // xy = scale, zw = rotation
    float4 uv_params;      // x = atlas_index, y = frame_progress, zw = padding
};

// Uniforms for particle rendering (push constants)
struct ParticleUniforms {
    float4x4 view_projection;
    float4x4 view_matrix;
    float time;
    float delta_time;
    uint blend_mode;
    uint texture_frames_x;
    uint texture_frames_y;
    float frame_rate;
};

// Vertex output
struct ParticleVertexOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float age;
};

// Quad vertices for instanced rendering (billboard)
constant float2 quad_positions[6] = {
    float2(-0.5, -0.5),
    float2( 0.5, -0.5),
    float2(-0.5,  0.5),
    float2(-0.5,  0.5),
    float2( 0.5, -0.5),
    float2( 0.5,  0.5)
};

constant float2 quad_uvs[6] = {
    float2(0.0, 0.0),
    float2(1.0, 0.0),
    float2(0.0, 1.0),
    float2(0.0, 1.0),
    float2(1.0, 0.0),
    float2(1.0, 1.0)
};

// Vertex shader for instanced particle rendering
vertex ParticleVertexOut particle_vertex_instanced(
    device const ParticleUniforms& uniforms [[buffer(0)]],
    device const ParticleData* particles [[buffer(1)]],
    device const uint* visible_count [[buffer(2)]],
    uint vertex_id [[vertex_id]],
    uint instance_id [[instance_id]])
{
    ParticleVertexOut out;
    
    // Cull instances beyond visible count
    if (instance_id >= visible_count[0]) {
        out.position = float4(0.0, 0.0, -1000.0, 1.0);
        out.color = float4(0.0);
        out.uv = float2(0.0);
        out.age = 1.0;
        return out;
    }
    
    // Get particle data - use instance_id directly as index
    device const ParticleData& p = particles[instance_id];
    
    float2 scale = p.scale_rotation.xy;
    float2 quad_pos = quad_positions[vertex_id];
    float2 quad_uv = quad_uvs[vertex_id];
    
    // Billboard: extract camera vectors from view matrix
    float3 camera_right = float3(uniforms.view_matrix[0][0], uniforms.view_matrix[1][0], uniforms.view_matrix[2][0]);
    float3 camera_up = float3(uniforms.view_matrix[0][1], uniforms.view_matrix[1][1], uniforms.view_matrix[2][1]);
    
    // Calculate world position
    float3 world_pos = p.position.xyz;
    float3 offset = camera_right * quad_pos.x * scale.x + camera_up * quad_pos.y * scale.y;
    
    float4 world_position = float4(world_pos + offset, 1.0);
    out.position = uniforms.view_projection * world_position;
    
    // Calculate texture UV from atlas index
    float atlas_index = p.uv_params.x;
    uint cols = uniforms.texture_frames_x;
    uint rows = uniforms.texture_frames_y;
    
    if (cols > 1 || rows > 1) {
        // Atlas mode - calculate UV offset
        float tile_x = fmod(atlas_index, float(cols));
        float tile_y = floor(atlas_index / float(cols));
        float2 tile_size = 1.0 / float2(float(cols), float(rows));
        float2 tile_offset = tile_size * float2(tile_x, tile_y);
        out.uv = tile_offset + quad_uv * tile_size;
    } else {
        // No atlas - use UV directly
        out.uv = quad_uv;
    }
    
    // Color with age-based alpha fade
    float age = p.position.w;
    float alpha_fade = 1.0 - smoothstep(0.7, 1.0, age);
    out.color = float4(p.color.rgb, p.color.a * alpha_fade);
    out.age = age;
    
    return out;
}

// Fragment shader for particle rendering
fragment float4 particle_fragment(
    ParticleVertexOut in [[stage_in]],
    texture2d<float> particle_texture [[texture(0)]],
    sampler tex_sampler [[sampler(0)]])
{
    // Sample texture
    constexpr sampler s(mip_filter::linear, address::clamp_to_edge);
    float4 tex_color = particle_texture.sample(s, in.uv);
    
    // Create circular shape from UV
    float2 centered = in.uv - 0.5;
    float dist = length(centered);
    
    if (dist > 0.5) {
        discard_fragment();
    }
    
    // Soft edge
    float alpha = 1.0 - smoothstep(0.3, 0.5, dist);
    
    // Combine texture with particle color
    return float4(in.color.rgb * tex_color.rgb, in.color.a * alpha * tex_color.a);
}
