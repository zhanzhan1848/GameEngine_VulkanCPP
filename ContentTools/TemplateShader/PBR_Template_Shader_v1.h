#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

    const char* PBR_Template_Vertex_Shader = "#version 450\n\
precision highp float;\n\
#include \"Common.h\" \n\
layout(location = 0) in vec4 inPos;\n\
layout(location = 1) in vec3 inColor;\n\
layout(location = 2) in vec2 inUV;\n\
layout(location = 3) in vec3 inNormal;\n\
layout(location = 4) in vec3 inTangent;\n\
layout(set = 0, binding = 0) uniform GlobalShaderDataBlock\n\
{\n\
    GlobalShaderData global_ubo;\n\
}global_ubo_block; \n\
layout(set = 0, binding = 1) uniform Model\n\
{\n\
    // Only guaranteed a total for 128 bytes\n\
    mat4 model; // 64 bytes\n\
    int mid;\n\
    int isReflect; \n\
} model;\n\
layout(location = 0) out struct dto\n\
{\n\
    vec3 position;\n\
    vec2 tex_coord;\n\
    vec3 normal;\n\
    vec3 view_position;\n\
    vec3 frag_position;\n\
    vec3 cameraDir;\n\
    vec4 color;\n\
    vec3 tangent;\n\
    float near;\n\
    float far;\n\
    float mid;\n\
    float isReflect; \n\
} out_dto;\n\
void main()\n\
{// Make sure to assign the shader id.\n\
    out_dto.tex_coord = inUV;\n\
    out_dto.color = vec4(inColor, 1.0); \n\
    // Fragment position in world space.\n\
    out_dto.frag_position = vec3(model.model * inPos);\n\
    // Copy the normal over.\n\
    mat3 m3_model = transpose(inverse(mat3(global_ubo_block.global_ubo.View * model.model)));\n\
    out_dto.normal = normalize(m3_model * inNormal); \n\
    out_dto.tangent = normalize(m3_model * inTangent);\n\
    out_dto.view_position = global_ubo_block.global_ubo.CameraPositon;\n\
    out_dto.cameraDir = global_ubo_block.global_ubo.CameraDirection;\n\
    gl_Position = global_ubo_block.global_ubo.Projection * global_ubo_block.global_ubo.View * model.model * inPos;\n\
    out_dto.position = vec3(model.model * inPos);\n\
    out_dto.near = global_ubo_block.global_ubo.NearPlane;\n\
    out_dto.far = global_ubo_block.global_ubo.FarPlane;\n\
    out_dto.mid = float(model.mid);\n\
    out_dto.isReflect = float(model.isReflect);\n\
}";

const char* PBR_Template_Fragment_Shader = "#version 450 \n\
#include \"Common.h\" \n\
#define Ambient vec4(0.25, 0.25, 0.25, 1.0)                  \n\
#define Ns {{Ns}} \n\
#define Ni {{Ni}} \n\
#define d {{d}}     \n\
#define Tr {{Tr}}  \n\
#define Tf {{Tf}}   \n\
#define Ka {{Ka}}   \n\
#define Kd {{Kd}}   \n\
#define Ks {{Ks}}   \n\
#define Ke {{Ke}}   \n\
#define diffuse_color {{diffuse_color}}                  \n\
#define shininess 8.0                            \n\
#define mode 0 \n\
layout(location = 0) out vec4 out_position; \n\
layout(location = 1) out vec4 out_normal; \n\
layout(location = 2) out vec4 out_color; \n\
layout(location = 3) out vec4 out_specular;\n\
// Samplers\n\
const int SAMP_DIFFUSE = 0;\n\
const int SAMP_SPECULAR = 1;\n\
const int SAMP_NORMAL = 2;\n\
{{images}}\n\
// Data Transfer Object\n\
layout(location = 0) in struct dto\n\
{\n\
    vec3 position;\n\
    vec2 tex_coord;\n\
    vec3 normal;\n\
    vec3 view_position;\n\
    vec3 frag_position;\n\
    vec3 cameraDir;\n\
    vec4 color;\n\
    vec3 tangent;\n\
    float near;\n\
    float far;\n\
    float mid;\n\
    float isReflect; \n\
} in_dto;\n\
mat3 TBN;\n\
float linearDepth(float depth)\n\
{\n\
    float z = depth * 2.0f - 1.0f;\n\
    return (2.0f * in_dto.near * in_dto.far) / (in_dto.far + in_dto.near - z * (in_dto.far - in_dto.near));\n\
}\n\
void main()\n\
{\n\
    vec3 normal = in_dto.normal;\n\
    vec3 tangent = in_dto.tangent;\n\
    tangent = (tangent - dot(tangent, normal) * normal);\n\
    vec3 bitangent = cross(in_dto.normal, in_dto.tangent);\n\
    TBN = mat3(tangent, bitangent, normal);\n\
    // Update the normal to use a sample from the normal map.\n\
    vec3 localNormal = 2.0 * texture(samplers[SAMP_NORMAL], in_dto.tex_coord).rgb - 1.0;\n\
    normal = normalize(TBN * localNormal);\n\
    vec3 view_direction = normalize(in_dto.view_position - in_dto.frag_position);\n\
    out_color = in_dto.color * vec4(diffuse_color, 1.0) * texture(samplers[SAMP_DIFFUSE], in_dto.tex_coord);\n\
    out_specular = vec4(texture(samplers[SAMP_SPECULAR], in_dto.tex_coord).rgb, in_dto.isReflect);\n\
    out_position = vec4(in_dto.frag_position, linearDepth(gl_FragCoord.z));\n\
    out_normal = vec4(normal, in_dto.mid / 100);\n\
}\n\
";

#if defined(__cplusplus)
}
#endif