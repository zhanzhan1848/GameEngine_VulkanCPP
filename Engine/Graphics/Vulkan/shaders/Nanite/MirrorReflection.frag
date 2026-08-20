#version 450 core
// MirrorReflection.frag — hand-written GLSL. Entry point: main
// (Metal: mirror_reflection_fs). Simple forward lambert + ambient; the
// reflection is composited over the main view later (MirrorComposite).

layout(std140, set = 0, binding = 0) uniform ReflectionParams {
    mat4 reflected_view_projection;
    vec4 light_dir;
    vec4 light_color;
    vec4 ambient;
};

layout(set = 0, binding = 9) uniform texture2DArray albedoTextures;
layout(set = 0, binding = 10) uniform sampler albedoSampler;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vNormalW;
layout(location = 2) in vec3 vAlbedoTint;
layout(location = 3) flat in uint vAlbedoIdx;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 albedo = texture(sampler2DArray(albedoTextures, albedoSampler),
                          vec3(vUV, float(vAlbedoIdx))).rgb * vAlbedoTint;

    float NdotL = max(dot(normalize(vNormalW), normalize(light_dir.xyz)), 0.0);
    vec3 color = albedo * (ambient.rgb + light_color.rgb * NdotL);

    outColor = vec4(color, 1.0);
}
