#version 450
#extension GL_ARB_shader_storage_buffer_object : require

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
    mat4 view_matrix;
    float time;
    float delta_time;
    uint vertex_count;
    uint frame_index;
} pc;

layout(location = 0) in vec3 in_position;  // Quad vertex positions (4 vertices)
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_uv;
layout(location = 2) out float out_age;
layout(location = 3) out flat uint out_texture_frame;

mat2 rotate2d(float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, -s, s, c);
}

void main() {
    uint instance_index = gl_InstanceIndex;
    
    uint particle_idx = visible_indices[instance_index];
    ParticleData p = particles[particle_idx];
    
    vec2 scale = p.scale_rotation.xy;
    float rotation = p.scale_rotation.z;
    
    // Billboard vectors from view matrix
    // For column-major matrices:
    vec3 camera_right = vec3(pc.view_matrix[0][0], pc.view_matrix[1][0], pc.view_matrix[2][0]);
    vec3 camera_up = vec3(pc.view_matrix[0][1], pc.view_matrix[1][1], pc.view_matrix[2][1]);
    
    // Apply rotation to UV coordinates
    mat2 rot = rotate2d(rotation);
    vec2 rotated_uv = rot * (in_uv - 0.5) + 0.5;
    out_uv = rotated_uv;
    
    // Calculate billboard position
    vec3 world_pos = p.position.xyz;
    vec3 offset = (camera_right * in_position.x * scale.x) + (camera_up * in_position.y * scale.y);
    
    vec4 world_position = vec4(world_pos + offset, 1.0);
    gl_Position = pc.view_projection * world_position;
    
    // Color with age-based alpha fade
    float age = p.position.w;
    float alpha_fade = 1.0 - smoothstep(0.7, 1.0, age);
    out_color = vec4(p.color.rgb, p.color.a * alpha_fade);
    out_age = age;
    
    // Texture animation (sprite sheet)
    // Frame is based on age and frame rate (stored in unused w component of scale_rotation)
    // For now, just output 0
    out_texture_frame = 0;
}
