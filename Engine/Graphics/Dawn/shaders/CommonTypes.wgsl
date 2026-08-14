// CommonTypes.wgsl — Ported from Metal CommonTypes.metal
// Shared structures for Dawn WebGPU shaders

struct GlobalShaderData {
    View:               mat4x4<f32>,
    Projection:         mat4x4<f32>,
    InvProjection:      mat4x4<f32>,
    ViewProjection:     mat4x4<f32>,
    PrevViewProjection: mat4x4<f32>,
    InvViewProjection:  mat4x4<f32>,
    CameraPositionAndViewWidth: vec4<f32>,  // xyz = position, w = viewWidth
    CameraDirectionAndViewHeight: vec4<f32>, // xyz = direction, w = viewHeight
    NumDirectionalLights: u32,
    DeltaTime: f32,
    FrameCount: f32,
    _padding: u32,
};

struct PerObjectData {
    World:               mat4x4<f32>,
    InvWorld:            mat4x4<f32>,
    WorldViewProjection: mat4x4<f32>,
    sh_coeffs:           array<vec4<f32>, 9>,
};

struct Surface {
    BaseColor:          vec3<f32>,
    Metallic:           f32,
    Normal:             vec3<f32>,
    PerceptualRoughness: f32,
    EmissiveColor:      vec3<f32>,
    EmissiveIntensity:  f32,
    AmbientOcclusion:   f32,
    // Pad to 16-byte alignment
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
};

struct LightParameters {
    Position:   vec3<f32>,
    Intensity:  f32,
    Direction:  vec3<f32>,
    Range:      f32,
    Color:      vec3<f32>,
    CosUmbra:   f32,
    Attenuation: vec3<f32>,
    CosPenumbra: f32,
};

struct DirectionalLightParameters {
    LightMVP:               mat4x4<f32>,
    DirectionAndIntensity:  vec4<f32>,
    Color:                  vec4<f32>,
};

struct SSAODispatchParameters {
    NumThreadGroups: vec2<u32>,
    NumThreads:      vec2<u32>,
};

struct SSGIDispatchParameters {
    NumThreadGroups: vec2<u32>,
    NumThreads:      vec2<u32>,
};
