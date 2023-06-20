#version 450

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec2 Texcoord;
layout(location = 1) in vec3 lightVec;
layout(location = 2) in vec3 viewVec;
layout(location = 3) in vec4 shadowCoord;

layout(binding = 1) uniform sampler2D texSampler;
layout(binding = 2) uniform sampler2D shadowMap;

#define ambient 0.1

float textureProj(vec4 coord, vec2 off)
{
    float shadow = 1.0;
    if(coord.z > -1.0 && coord.z < 1.0)
    {
        float dist = texture(shadowMap, coord.st + off).r;
        if(coord.w > 0.0 && dist < coord.z)
        {
            shadow = ambient;
        }
    }
    return shadow;
}

float LinearizeDepth(float depth)
{
    float n = 0.001;
    float f = 10.0;
    float z = depth;
    return (2.0 * n) / (f + n - z * (f - n));
}

void main() {
    
    float shadow = textureProj(shadowCoord / shadowCoord.w, vec2(0.0));
    vec3 N = vec3(0.0, 1.0, 0.0);
    vec3 L = normalize(lightVec);
    vec3 V = normalize(viewVec);
    vec3 R = normalize(-reflect(L, N));
    vec3 diffuse = max(dot(N, L), ambient) * vec3(1.0, 1.0, 1.0);

    float depth = texture(shadowMap, Texcoord).r;
    //outColor = vec4(vec3(1.0 - ((2.0 * 0.001) / (10.0 + 0.001 - depth * (10.0 - 0.001)))), 1.0);
    //outColor = vec4(smoothstep(0.0, 1.0, texture(shadowMap, Texcoord2).r)); //
    outColor = vec4(diffuse * shadow, 1.0);
}