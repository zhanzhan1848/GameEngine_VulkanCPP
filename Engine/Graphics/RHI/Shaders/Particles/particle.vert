#version 450
#extension GL_ARB_shader_storage_buffer_object : require
#extension GL_ARB_draw_instanced : require

struct ParticleData {
    vec4 position;      // xyz = position, w = age (0-1)
    vec4 velocity;      // xyz = velocity, w = lifetime
    vec4 color;         // rgba
    vec4 scale_rotation; // xy = scale, zw = rotation
};

layout(std430, binding = 0) readonly buffer VisibleIndices {
    uint visible_indices[];
};

layout(std430, binding = 1) readonly buffer ParticleBuffer {
    ParticleData particles[];
};

layout(push_constant) uniform PushConstants {
    mat4 view_projection;
    float time;
    float delta_time;
    uint vertex_count;
    uint _pad;
} pc;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_uv;
layout(location = 2) out float out_age;

// Full-screen quad vertices
const vec2 quad_vertices[6] = vec2[](
    vec2(-0.5, -0.5),
    vec2( 0.5, -0.5),
    vec2(-0.5,  0.5),
    vec2(-0.5,  0.5),
    vec2( 0.5, -0.5),
    vec2( 0.5,  0.5)
);

mat2 rotate2d(float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, -s, s, c);
}

void main() {
    uint instance_index = gl_InstanceIndex;
    uint vertex_index = gl_VertexIndex;
    
    // Get visible particle index
    uint particle_idx = visible_indices[instance_index];
    ParticleData p = particles[particle_idx];
    
    // Calculate UV coordinates for this vertex
    vec2 quad_pos = quad_vertices[vertex_index];
    out_uv = quad_pos + 0.5;
    
    // Apply rotation and scale
    float rotation = p.scale_rotation.z;
    vec2 scale = p.scale_rotation.xy;
    
    mat2 rot = rotate2d(rotation);
    vec2 transformed_pos = rot * (quad_pos * scale);
    
    // Billboard: face the camera
    // Extract right and up vectors from view matrix (assumed to be in push constants or separate uniform)
    // For now, simple screen-aligned quad
    vec3 world_pos = p.position.xyz;
    
    // Get camera right and up from view matrix
    // View matrix is the inverse of camera transform
    // Right = view[0].xyz, Up = view[1].xyz (if view is row-major)
    // For column-major: Right = view[0][0], view[1][0], view[2][0]
    
    // Simple approach: just offset in world space
    // In a real implementation, you'd extract billboarding vectors from view matrix
    vec3 offset = vec3(transformed_pos, 0.0);
    
    vec4 world_position = vec4(world_pos + offset, 1.0);
    
    gl_Position = pc.view_projection * world_position;
    
    // Output particle color with alpha fade based on age
    float age = p.position.w;
    float alpha_fade = 1.0 - smoothstep(0.8, 1.0, age);
    out_color = vec4(p.color.rgb, p.color.a * alpha_fade);
    out_age = age;
}
