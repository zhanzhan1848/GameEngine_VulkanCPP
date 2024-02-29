#version 450
precision highp float;
#include "Common.h" 
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec3 inTangent;
layout(set = 0, binding = 0) uniform GlobalShaderDataBlock
{
    GlobalShaderData global_ubo;
}global_ubo_block; 
layout(set = 0, binding = 1) uniform Model
{
    // Only guaranteed a total for 128 bytes
    mat4 model; // 64 bytes
    int mid;
    int isReflect; 
} model;
layout(location = 0) out struct dto
{
    vec3 position;
    vec2 tex_coord;
    vec3 normal;
    vec3 view_position;
    vec3 frag_position;
    vec3 cameraDir;
    vec4 color;
    vec3 tangent;
    float near;
    float far;
    float mid;
    float isReflect; 
} out_dto;
void main()
{// Make sure to assign the shader id.
    out_dto.tex_coord = inUV;
    out_dto.color = vec4(inColor, 1.0); 
    // Fragment position in world space.
    out_dto.frag_position = vec3(model.model * inPos);
    // Copy the normal over.
    mat3 m3_model = transpose(inverse(mat3(global_ubo_block.global_ubo.View * model.model)));
    out_dto.normal = normalize(m3_model * inNormal); 
    out_dto.tangent = normalize(m3_model * inTangent);
    out_dto.view_position = global_ubo_block.global_ubo.CameraPositon;
    out_dto.cameraDir = global_ubo_block.global_ubo.CameraDirection;
    gl_Position = global_ubo_block.global_ubo.Projection * global_ubo_block.global_ubo.View * model.model * inPos;
    out_dto.position = vec3(model.model * inPos);
    out_dto.near = global_ubo_block.global_ubo.NearPlane;
    out_dto.far = global_ubo_block.global_ubo.FarPlane;
    out_dto.mid = float(model.mid);
    out_dto.isReflect = float(model.isReflect);
}