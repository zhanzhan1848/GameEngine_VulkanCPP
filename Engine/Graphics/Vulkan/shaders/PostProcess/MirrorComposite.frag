#version 450 core
// MirrorComposite.frag — hand-written GLSL. Entry point: main
// (Metal: mirror_composite_fs). Projects the mirror pixel through the
// reflected VP and samples the reflection RT; blends over the HDR frame
// with a view-angle Fresnel term. Blending: SrcAlpha / InvSrcAlpha.

layout(std140, set = 0, binding = 0) uniform CompositeParams {
    mat4 view_projection;
    mat4 reflected_view_projection;
    vec4 plane_position;
    vec4 plane_normal;
    vec4 plane_axes;
    vec4 camera_position;   // w = reflectivity
};

layout(set = 0, binding = 1) uniform texture2D reflectionTex;
layout(set = 0, binding = 2) uniform sampler reflectionSampler;

layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;

void main() {
    // Project the world pixel through the REFLECTED VP. The reflection RT is
    // rasterized by raw GLSL (MirrorReflection.vert — NO naga Y-flip), so its
    // V axis follows the plain Vulkan convention v = ndc.y*0.5 + 0.5.
    vec4 clip = reflected_view_projection * vec4(vWorldPos, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);

    // Off-mirror guard (quad is exact, but fp noise at borders).
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) discard;

    vec3 reflection = texture(sampler2D(reflectionTex, reflectionSampler), uv).rgb;

    // View-angle Fresnel: grazing views reflect more.
    vec3 viewDir = normalize(vWorldPos - camera_position.xyz);
    float cosTheta = abs(dot(viewDir, normalize(plane_normal.xyz)));
    float fresnel = mix(0.35, 0.9, pow(1.0 - cosTheta, 3.0));

    float alpha = clamp(camera_position.w * fresnel, 0.0, 1.0);
    outColor = vec4(reflection, alpha);
}
