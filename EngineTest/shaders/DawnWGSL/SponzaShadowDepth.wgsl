// Indexed Sponza geometry is already in world space. Four distinct UBOs
// prevent per-cascade queue uploads from overwriting earlier recorded draws.
struct ShadowData { lightVP: mat4x4<f32>, };
@group(0) @binding(0) var<uniform> shadow: ShadowData;
@vertex fn shadow_depth(@location(0) position:vec3<f32>)->@builtin(position) vec4<f32>{
    var clip=shadow.lightVP*vec4(position,1);
    clip.z+=.001; // Frozen Vulkan Nanite/ShadowDepth.wgsl caster bias.
    return clip;
}
