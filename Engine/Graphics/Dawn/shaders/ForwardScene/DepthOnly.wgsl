// DepthOnly.wgsl — shadow mapping depth-only pass for ForwardSceneRenderer.
// Ported from Engine/Graphics/Metal/shaders/Forward/DepthOnly.metal.
//
// Pipeline layout (Dawn/WGSL):
//   @group(0) @binding(0)  var<uniform> viewData: ViewData
//   @group(0) @binding(1)  var<uniform> sceneData: SceneData   (declared, unused here)
//   @group(1)              material descriptor set (unused for depth-only)
//   @group(2) @binding(0)  var<uniform> pushConsts: PCGPushConsts
//
// Vertex input (slot 0, 32B stride, per-vertex):
//   @location(0) position: vec3<f32>
//   @location(1..4) unused for this shader (color_t_sign/normal/tangent/uv)
//
// Instance input (slot 1, 96B stride, per-instance):
//   @location(5..8) transform columns (mat4x4 reconstructed from 4 vec4s)
//   @location(9)    baseColor (unused)
//   @location(10)   roughness/metallic/alphaCutoff/pad (unused)

struct ViewData {
    viewProjection: mat4x4<f32>,
    invViewProjection: mat4x4<f32>,
    previousViewProjection: mat4x4<f32>,
};

struct PCGPushConsts {
    transform: mat4x4<f32>,
    use_instances: u32,
    _pad0: u32,
    _pad1: u32,
    _pad2: u32,
};

@group(0) @binding(0) var<uniform> viewData: ViewData;
@group(2) @binding(0) var<uniform> pushConsts: PCGPushConsts;

struct VSOut {
    @builtin(position) position: vec4<f32>,
};

@vertex
fn shadow_mapping_vs(
    @location(0) position: vec3<f32>,
    @location(5) t_col0: vec4<f32>,
    @location(6) t_col1: vec4<f32>,
    @location(7) t_col2: vec4<f32>,
    @location(8) t_col3: vec4<f32>,
) -> VSOut {
    var out: VSOut;
    let model = mat4x4<f32>(t_col0, t_col1, t_col2, t_col3);
    let worldPos = model * vec4<f32>(position, 1.0);
    out.position = viewData.viewProjection * worldPos;
    return out;
}
