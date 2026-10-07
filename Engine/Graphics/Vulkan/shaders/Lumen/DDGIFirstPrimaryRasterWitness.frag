#version 450

// Private DDGI first-hit raster witness.  This fragment is never bound by a
// production pass.  It consumes the locations emitted by the actual
// GPUDrivenDraw vertex shader and writes only float32 diagnostic attachments.

layout(std140, set = 0, binding = 0) uniform DrawConstants {
    mat4 view_matrix;
    mat4 proj_matrix;
    mat4 world_matrix;
    uint view_width;
    uint view_height;
    uint meshlet_count;
    uint padding;
    mat4 prev_view_matrix;
    mat4 prev_proj_matrix;
    uint has_prev_frame;
    uint debug_mode;
    uint pad2_1;
    uint pad2_2;
    vec2 jitter;
    vec2 prev_jitter;
} dc;

layout(std430, set = 0, binding = 9) readonly buffer MaterialData {
    uint words[];
} material_data;

layout(set = 0, binding = 11) uniform texture2DArray normal_textures;
layout(set = 0, binding = 13) uniform sampler texture_sampler;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_tangent;
layout(location = 3) in vec3 in_bitangent;
layout(location = 4) flat in uint in_material_id;
layout(location = 5) flat in uint in_meshlet_id;
layout(location = 6) flat in uint in_triangle_id;
layout(location = 7) flat in uint in_mesh_id;
layout(location = 8) in vec4 in_current_clip;
layout(location = 9) in vec4 in_previous_clip;
layout(location = 10) in vec3 in_object_normal;
layout(location = 11) in vec3 in_world_pos;

layout(location = 0) out vec4 witness_position;
layout(location = 1) out vec4 witness_uv_material;
layout(location = 2) out vec4 witness_normal;
layout(location = 3) out vec4 witness_tangent;
layout(location = 4) out vec4 witness_derivative;
layout(location = 5) out vec4 witness_normal_sample;
layout(location = 6) out vec4 witness_current_clip;
layout(location = 7) out vec4 witness_fragment_coord;

void main() {
    // W0: interpolated world position and private frame stamp.
    witness_position = vec4(in_world_pos, uintBitsToFloat(dc.pad2_1));
    // Separate actual original VS varying and native fragment built-in.
    // These are not reconstructed from P or a CPU projection.
    witness_current_clip = in_current_clip;
    witness_fragment_coord = gl_FragCoord;

    // W1/W2/W3: the actual primary VS interpolants.  Integer IDs are encoded
    // with uintBitsToFloat so the readback can distinguish them from color.
    witness_uv_material = vec4(in_uv, uintBitsToFloat(in_material_id),
                               uintBitsToFloat(dc.pad2_2));
    witness_normal = vec4(in_normal, uintBitsToFloat(in_triangle_id));
    witness_tangent = vec4(in_tangent, uintBitsToFloat(in_meshlet_id));

    // W4 is the actual geometric derivative normal from the fragment quad.
    // Its alpha carries the exact flat triangle id; the host treats a
    // degenerate/non-finite normal as UNKNOWN and never replaces it on CPU.
    vec3 geom = normalize(cross(dFdy(in_world_pos), dFdx(in_world_pos)));
    witness_derivative = vec4(geom, uintBitsToFloat(in_triangle_id));

    // W5: same material normal-array lookup used by the original fragment.
    // MaterialData is 12 uint words per record; malformed IDs use a neutral
    // normalSample and remain visible as an ID mismatch in the host reader.
    vec3 normalSample = vec3(-2.0, -2.0, -2.0);
    uint base = in_material_id * 12u;
    if (in_material_id != 0xffffffffu) {
        uint normal_index = material_data.words[base + 1u];
        vec2 uv_scale = vec2(uintBitsToFloat(material_data.words[base + 9u]),
                             uintBitsToFloat(material_data.words[base + 10u]));
        ivec3 dimensions = textureSize(sampler2DArray(normal_textures, texture_sampler), 0);
        if (normal_index != 0xffffffffu && normal_index < uint(max(dimensions.z, 0))) {
            normalSample = textureLod(sampler2DArray(normal_textures, texture_sampler),
                                vec3(fract(in_uv * uv_scale), float(normal_index)), 0.0).xyz;
        }
    }
    witness_normal_sample = vec4(normalSample, uintBitsToFloat(in_mesh_id));
}
