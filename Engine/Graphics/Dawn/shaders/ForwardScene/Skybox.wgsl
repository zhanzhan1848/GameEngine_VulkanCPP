// Skybox.wgsl — ForwardScene variant of Forward/Skybox.metal.
//
// Renders a unit cube at camera position with depth forced to far plane.
// The texture_cube samples along the view ray direction.
//
// Pipeline layout (Dawn/WGSL): single descriptor set at group(0).
//   @group(0) @binding(0)  var<uniform> viewData: ViewData
//   @group(0) @binding(1)  var<uniform> sceneData: SceneData
//   @group(0) @binding(2)  var skyboxTex: texture_cube<f32>
//   @group(0) @binding(3)  var skyboxSampler: sampler

struct ViewData {
    viewProjection: mat4x4<f32>,
    invViewProjection: mat4x4<f32>,
    previousViewProjection: mat4x4<f32>,
};

struct SceneData {
    model: mat4x4<f32>,
    lightPos: vec4<f32>,
    lightColor: vec4<f32>,
    reflectionPlane: vec4<f32>,
    reflectionPlane2: vec4<f32>,
    reflectionPlane3: vec4<f32>,
    previousModel: mat4x4<f32>,
    jitter: vec2<f32>,
    previousJitter: vec2<f32>,
    time: f32,
    _timePad: f32,
    viewPos: vec4<f32>,
    shadowMatrix0: mat4x4<f32>,
    shadowMatrix1: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> viewData: ViewData;
@group(0) @binding(1) var<uniform> sceneData: SceneData;
@group(0) @binding(2) var skyboxTex: texture_cube<f32>;
@group(0) @binding(3) var skyboxSampler: sampler;

// Unit cube vertices (36 vertices, 6 per face × 6 faces). Matches Metal.
var<private> cubeVertices: array<vec3<f32>, 36> = array<vec3<f32>, 36>(
    // Back face (-Z)
    vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(-1.0,  1.0, -1.0), vec3<f32>( 1.0,  1.0, -1.0),
    vec3<f32>( 1.0,  1.0, -1.0), vec3<f32>( 1.0, -1.0, -1.0), vec3<f32>(-1.0, -1.0, -1.0),
    // Front face (+Z)
    vec3<f32>(-1.0, -1.0,  1.0), vec3<f32>( 1.0, -1.0,  1.0), vec3<f32>( 1.0,  1.0,  1.0),
    vec3<f32>( 1.0,  1.0,  1.0), vec3<f32>(-1.0,  1.0,  1.0), vec3<f32>(-1.0, -1.0,  1.0),
    // Left face (-X)
    vec3<f32>(-1.0,  1.0,  1.0), vec3<f32>(-1.0,  1.0, -1.0), vec3<f32>(-1.0, -1.0, -1.0),
    vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(-1.0, -1.0,  1.0), vec3<f32>(-1.0,  1.0,  1.0),
    // Right face (+X)
    vec3<f32>( 1.0,  1.0,  1.0), vec3<f32>( 1.0, -1.0,  1.0), vec3<f32>( 1.0, -1.0, -1.0),
    vec3<f32>( 1.0, -1.0, -1.0), vec3<f32>( 1.0,  1.0, -1.0), vec3<f32>( 1.0,  1.0,  1.0),
    // Bottom face (-Y)
    vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>( 1.0, -1.0, -1.0), vec3<f32>( 1.0, -1.0,  1.0),
    vec3<f32>( 1.0, -1.0,  1.0), vec3<f32>(-1.0, -1.0,  1.0), vec3<f32>(-1.0, -1.0, -1.0),
    // Top face (+Y)
    vec3<f32>(-1.0,  1.0, -1.0), vec3<f32>(-1.0,  1.0,  1.0), vec3<f32>( 1.0,  1.0,  1.0),
    vec3<f32>( 1.0,  1.0,  1.0), vec3<f32>( 1.0,  1.0, -1.0), vec3<f32>(-1.0,  1.0, -1.0)
);

struct VSOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uvw: vec3<f32>,
};

@vertex
fn vertexSkybox(@builtin(vertex_index) vid: u32) -> VSOut {
    var out: VSOut;
    let pos = cubeVertices[vid];
    out.uvw = pos;

    // Translate cube to camera position so the skybox follows the viewer.
    let camPos = sceneData.viewPos.xyz;
    let worldPos = vec4<f32>(pos + camPos, 1.0);
    let clip = viewData.viewProjection * worldPos;

    // Force depth to far plane (NDC z = 1 in WebGPU [0,1] range).
    // Setting clip.z = clip.w makes NDC z = 1 after perspective divide.
    out.position = vec4<f32>(clip.xy, clip.w, clip.w);
    return out;
}

struct FSOut {
    @location(0) color: vec4<f32>,
};

@fragment
fn fragmentSkybox(in: VSOut) -> FSOut {
    var out: FSOut;
    out.color = textureSample(skyboxTex, skyboxSampler, in.uvw);
    return out;
}
