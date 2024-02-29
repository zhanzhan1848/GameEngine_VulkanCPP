#version 450 
#include "Common.h" 
#define Ambient vec4(0.25, 0.25, 0.25, 1.0)                  
#define Ns 10.000000 
#define Ni 1.500000 
#define d 1.000000     
#define Tr 0.000000  
#define Tf vec3(1.000000,1.000000,1.000000)   
#define Ka vec3(0.588000,0.588000,0.588000)   
#define Kd vec3(0.588000,0.588000,0.588000)   
#define Ks vec3(0.000000,0.000000,0.000000)   
#define Ke vec3(0.000000,0.000000,0.000000)   
#define diffuse_color vec3(0.588000,0.588000,0.588000)                  
#define shininess 8.0                            
#define mode 0 
layout(location = 0) out vec4 out_position; 
layout(location = 1) out vec4 out_normal; 
layout(location = 2) out vec4 out_color; 
layout(location = 3) out vec4 out_specular;
// Samplers
const int SAMP_DIFFUSE = 0;
const int SAMP_SPECULAR = 1;
const int SAMP_NORMAL = 2;

// Data Transfer Object
layout(location = 0) in struct dto
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
} in_dto;
mat3 TBN;
float linearDepth(float depth)
{
    float z = depth * 2.0f - 1.0f;
    return (2.0f * in_dto.near * in_dto.far) / (in_dto.far + in_dto.near - z * (in_dto.far - in_dto.near));
}
void main()
{
    vec3 normal = in_dto.normal;
    vec3 tangent = in_dto.tangent;
    tangent = (tangent - dot(tangent, normal) * normal);
    vec3 bitangent = cross(in_dto.normal, in_dto.tangent);
    TBN = mat3(tangent, bitangent, normal);
    // Update the normal to use a sample from the normal map.
    vec3 localNormal = 2.0 * vec3(1.0) - 1.0;
    normal = normalize(TBN * localNormal);
    vec3 view_direction = normalize(in_dto.view_position - in_dto.frag_position);
    out_color = in_dto.color * vec4(diffuse_color, 1.0) * vec4(1.0);
    out_specular = vec4(vec3(1.0), in_dto.isReflect);
    out_position = vec4(in_dto.frag_position, linearDepth(gl_FragCoord.z));
    out_normal = vec4(normal, in_dto.mid / 100);
}
