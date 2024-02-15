#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

const char* PBR_Template_Vertex_Shader = "#version 450\n\
layout(location = 0) in vec4 inPos;\n\
layout(location = 1) in vec3 inColor;\n\
layout(location = 2) in vec2 inUV;\n\
layout(location = 3) in vec3 inNormal;\n\
layout(location = 4) in vec3 inTangent;\n\
layout(set = 0, binding = 0) uniform global_uniform_object\n\
{\n\
    mat4 model; \n\
    mat4 view;\n\
    mat4 projection;\n\
    vec3 view_position;\n\
    vec3 cameraDir; \n\
    float far; \n\
    float near; \n\
    float time; \n\
} global_ubo;\n\
layout(set = 0, binding = 1) uniform Model\n\
{\n\
    // Only guaranteed a total for 128 bytes\n\
    mat4 model; // 64 bytes\n\
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
} out_dto;\n\
void main()\n\
{// Make sure to assign the shader id.\n\
    out_dto.tex_coord = inUV;\n\
    out_dto.color = vec4(inColor, 1.0); \n\
    // Fragment position in world space.\n\
    out_dto.frag_position = vec3(model.model * inPos);\n\
    // Copy the normal over.\n\
    mat3 m3_model = transpose(inverse(mat3(global_ubo.view * model.model)));\n\
    out_dto.normal = normalize(m3_model * inNormal); \n\
    out_dto.tangent = normalize(m3_model * inTangent);\n\
    out_dto.view_position = global_ubo.view_position;\n\
    out_dto.cameraDir = global_ubo.cameraDir;\n\
    gl_Position = global_ubo.projection * global_ubo.view * model.model * inPos;\n\
    out_dto.position = vec3(model.model * inPos);\n\
    out_dto.near = global_ubo.near;\n\
    out_dto.far = global_ubo.far;\n\
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
struct point_light {\n\
    vec3 position;\n\
    vec4 color;\n\
    // Usually 1, make sure denominator never gets smaller than 1\n\
    float constant; \n\
    // Reduces light intensity linearly\n\
    float linear;\n\
    // Makes the light fall off slower at longer distances.\n\
    float quadratic;\n\
};\n\
// TODO: feed in from cpu\n\
point_light p_light_0 = {\n\
    vec3(-5.5, 0.0, -5.5),\n\
    vec4(0.0, 1.0, 0.0, 1.0),\n\
    3.0, // Constant\n\
    0.35, // Linear\n\
    0.44  // Quadratic\n\
};\n\
// TODO: feed in from cpu\n\
point_light p_light_1 = {\n\
    vec3(5.5, 0.0, -5.5),\n\
    vec4(1.0, 0.0, 0.0, 1.0),\n\
    3.0, // Constant\n\
    0.35, // Linear\n\
    0.44  // Quadratic\n\
};\n\
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
} in_dto;\n\
layout(set = 1, binding = 0) uniform DirectionalLightParameter\n\
{\n\
    DirectionalLightParameters param[256];\n\
} directionalLight;\n\
layout(push_constant) uniform light_nums\n\
{\n\
    int light_num; \n\
} light_Nums;\n\
mat3 TBN;\n\
vec4 calculate_directional_light(vec3 normal);\n\
vec4 calculate_directional_light(DirectionalLightParameters light, vec3 normal, vec3 view_direction);\n\
vec4 calculate_point_light(point_light light, vec3 normal, vec3 frag_position, vec3 view_direction);\n\
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
    out_color = in_dto.color;\n\
    for(int i = 0; i < light_Nums.light_num; ++i)\n\
    {\n\
        vec3 lightDir = directionalLight.param[i].Direction;\n\
        if(abs(lightDir.z - 1.0) < 0.001) lightDir = in_dto.cameraDir;\n\
        //out_color += calculate_directional_light(directionalLight.param[i], normal, view_direction);\n\
        float diffuse_1 = max(dot(normal, -lightDir), 0.0);\n\
        vec3 reflection_1 = reflect(lightDir, normal);\n\
        float specular_1 = pow(max(dot(in_dto.cameraDir, reflection_1), 0.0), shininess) * 0.5;\n\
        vec3 lightColor_1 = directionalLight.param[i].Color * directionalLight.param[i].Intensity;\n\
        out_color += vec4((diffuse_1 + specular_1) * lightColor_1, 1.0);\n\
    }\n\
    out_color = clamp((out_color / 3.0) + (10 / 255.0), 0.0, 1.0);\n\
    out_color = texture(samplers[SAMP_DIFFUSE], in_dto.tex_coord) * out_color;\n\
    //out_color += calculate_directional_light(normal);\n\
    out_color += calculate_point_light(p_light_0, normal, in_dto.frag_position, view_direction);\n\
    out_color += calculate_point_light(p_light_1, normal, in_dto.frag_position, view_direction);\n\
    out_position = vec4(in_dto.frag_position, linearDepth(gl_FragCoord.z));\n\
    out_normal = vec4(normal, 1.0);\n\
}\n\
vec4 calculate_directional_light(vec3 normal) {\n\
    float diffuse_factor = 0.0;\n\
    float specular_factor = 0.0;\n\
    vec4 lightColor = vec4(0.0);\n\
    for(int i = 0; i < light_Nums.light_num; ++i)\n\
    {\n\
        vec3 lightDir = directionalLight.param[i].Direction;\n\
        if(abs(lightDir.z - 1.0) < 0.001) lightDir = in_dto.cameraDir;\n\
        diffuse_factor += max(dot(normal, lightDir), 0.0);\n\
        vec3 reflection = reflect(lightDir, normal);\n\
        specular_factor += pow(max(dot(in_dto.cameraDir, reflection), 0.0), shininess) * 0.5;\n\
        lightColor += vec4(directionalLight.param[i].Color * directionalLight.param[i].Intensity, 1.0);\n\
        //vec3 half_direction = normalize(view_direction - directionalLight.param[i].Direction);\n\
        //specular_factor += pow(max(dot(half_direction, normal), 0.0), shininess);\n\
    }\n\
    diffuse_factor = clamp(diffuse_factor + (10 / 255.0), 0.0, 1.0);\n\
    specular_factor = clamp(specular_factor + (10 / 255.0), 0.0, 1.0);\n\
    lightColor = clamp(lightColor + (10 / 255.0), 0.0, 1.0);\n\
    vec4 diff_samp = texture(samplers[SAMP_DIFFUSE], in_dto.tex_coord);\n\
    vec4 ambient = vec4(vec3(Ambient.xyz * diffuse_color), diff_samp.a);\n\
    vec4 diffuse = vec4(vec3(lightColor * diffuse_factor), diff_samp.a);\n\
    vec4 specular = vec4(vec3(lightColor * specular_factor), diff_samp.a);\n\
    diffuse *= diff_samp;\n\
    ambient *= diff_samp;\n\
    specular *= vec4(texture(samplers[SAMP_SPECULAR], in_dto.tex_coord).rgb, diffuse.a);\n\
    return clamp(ambient + diffuse + specular, 0.0, 1.0);\n\
}\n\
vec4 calculate_directional_light(DirectionalLightParameters light, vec3 normal, vec3 view_direction) {\n\
    float diffuse_factor = max(dot(normal, -light.Direction), 0.0);\n\
    vec3 half_direction = normalize(view_direction - light.Direction);\n\
    float specular_factor = pow(max(dot(half_direction, normal), 0.0), shininess);\n\
    vec4 diff_samp = texture(samplers[SAMP_DIFFUSE], in_dto.tex_coord);\n\
    vec4 ambient = vec4(vec3(Ambient.xyz * diffuse_color), diff_samp.a);\n\
    vec4 diffuse = vec4(vec3(light.Color * diffuse_factor), diff_samp.a);\n\
    vec4 specular = vec4(vec3(light.Color * specular_factor), diff_samp.a);\n\
    diffuse *= diff_samp;\n\
    ambient *= diff_samp;\n\
    specular *= vec4(texture(samplers[SAMP_SPECULAR], in_dto.tex_coord).rgb, diffuse.a);\n\
    return (ambient + diffuse + specular) * (vec4(light.Color, 1.0) * light.Intensity);\n\
}\n\
vec4 calculate_point_light(point_light light, vec3 normal, vec3 frag_position, vec3 view_direction) {\n\
    vec3 light_direction = normalize(light.position - frag_position);\n\
    float diff = max(dot(normal, light_direction), 0.0);\n\
    vec3 reflect_direction = reflect(-light_direction, normal);\n\
    float spec = pow(max(dot(view_direction, reflect_direction), 0.0), shininess);\n\
    // Calculate attenuation, or light falloff over distance.\n\
    float distance = length(light.position - frag_position);\n\
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));\n\
    vec4 ambient = Ambient;\n\
    vec4 diffuse = light.color * diff;\n\
    vec4 specular = light.color * spec;\n\
    vec4 diff_samp = texture(samplers[SAMP_DIFFUSE], in_dto.tex_coord);\n\
    diffuse *= diff_samp;\n\
    ambient *= diff_samp;\n\
    specular *= vec4(texture(samplers[SAMP_SPECULAR], in_dto.tex_coord).rgb, diffuse.a);\n\
    ambient *= attenuation;\n\
    diffuse *= attenuation;\n\
    specular *= attenuation;\n\
    return (ambient + diffuse + specular); \n\
} ";

#if defined(__cplusplus)
}
#endif