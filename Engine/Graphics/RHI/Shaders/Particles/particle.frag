#version 450

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in float in_age;
layout(location = 3) in flat uint in_texture_frame;

layout(binding = 0) uniform sampler2D particle_texture;

layout(push_constant) uniform PushConstants {
    mat4 view_projection;
    float time;
    float delta_time;
    uint blend_mode;    // 0=additive, 1=alpha, 2=multiply, 3=premultiplied
    uint texture_frames_x;
    uint texture_frames_y;
    float frame_rate;
} pc;

layout(location = 0) out vec4 out_color;

vec2 calculate_sprite_sheet_uv(vec2 uv, uint frame) {
    if (pc.texture_frames_x <= 1 && pc.texture_frames_y <= 1) {
        return uv;
    }
    
    uint frames_per_row = pc.texture_frames_x;
    uint frame_x = frame % frames_per_row;
    uint frame_y = frame / frames_per_row;
    
    vec2 frame_size = vec2(1.0) / vec2(float(pc.texture_frames_x), float(pc.texture_frames_y));
    
    return vec2(frame_x, frame_y) * frame_size + uv * frame_size;
}

void main() {
    // Calculate UV for sprite sheet animation
    vec2 tex_uv = calculate_sprite_sheet_uv(in_uv, in_texture_frame);
    
    vec4 tex_color = texture(particle_texture, tex_uv);
    
    // Apply vertex color modulation
    vec4 final_color = tex_color * in_color;
    
    // Discard fully transparent pixels for performance
    if (final_color.a < 0.01) {
        discard;
    }
    
    out_color = final_color;
}
