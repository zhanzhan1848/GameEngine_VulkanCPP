#version 450 core

// T4.6.5 part 2 — Vulkan port of Forward/Skybox.metal
//
// Skybox vertex shader. Entry point: vertexSkybox
// Generates a unit cube from gl_VertexIndex (no vertex buffer needed).
//
// Descriptor set layout (matches ForwardSceneRenderer::skybox_set_layout_):
//   set 3 (skybox): binding 0 = ViewData UBO, binding 1 = SceneData UBO
//
// Push constants: none (uses UBOs only)

// Match ForwardSceneRenderer::CreateDescriptorLayouts "Global set" — both UBOs at set 0
// Skybox pipeline creates its own layout: skybox_set_layout_ with 4 bindings.
// The actual set number depends on how the pipeline is wired; use set 0 here
// (ForwardSceneRenderer binds skybox_set_layout_ as set 0 in the pipeline layout).

layout(set = 0, binding = 0) uniform ViewData {
    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 invViewProjection;
} viewData;

layout(set = 0, binding = 1) uniform SceneData {
    mat4 model;
    vec4 lightPos;
    vec4 lightColor;
    vec4 reflectionPlane;
    vec4 reflectionPlane2;
    vec4 reflectionPlane3;
    mat4 previousModel;
    vec2 jitter;
    vec2 previousJitter;
    vec2 padding;
    vec4 viewPos;
    mat4 shadowMatrix0;
    mat4 shadowMatrix1;
} sceneData;

layout(location = 0) out vec3 outUv;

// 36-vertex unit cube (matches Metal Skybox.metal:31 cubeVertices)
const vec3 cubeVertices[36] = vec3[36](
    // Back face
    vec3(-1.0, -1.0, -1.0), vec3(-1.0,  1.0, -1.0), vec3( 1.0,  1.0, -1.0),
    vec3( 1.0,  1.0, -1.0), vec3( 1.0, -1.0, -1.0), vec3(-1.0, -1.0, -1.0),
    // Front face
    vec3(-1.0, -1.0,  1.0), vec3( 1.0, -1.0,  1.0), vec3( 1.0,  1.0,  1.0),
    vec3( 1.0,  1.0,  1.0), vec3(-1.0,  1.0,  1.0), vec3(-1.0, -1.0,  1.0),
    // Left face
    vec3(-1.0,  1.0,  1.0), vec3(-1.0,  1.0, -1.0), vec3(-1.0, -1.0, -1.0),
    vec3(-1.0, -1.0, -1.0), vec3(-1.0, -1.0,  1.0), vec3(-1.0,  1.0,  1.0),
    // Right face
    vec3( 1.0,  1.0,  1.0), vec3( 1.0, -1.0,  1.0), vec3( 1.0, -1.0, -1.0),
    vec3( 1.0, -1.0, -1.0), vec3( 1.0,  1.0, -1.0), vec3( 1.0,  1.0,  1.0),
    // Bottom face
    vec3(-1.0, -1.0, -1.0), vec3( 1.0, -1.0, -1.0), vec3( 1.0, -1.0,  1.0),
    vec3( 1.0, -1.0,  1.0), vec3(-1.0, -1.0,  1.0), vec3(-1.0, -1.0, -1.0),
    // Top face
    vec3(-1.0,  1.0, -1.0), vec3(-1.0,  1.0,  1.0), vec3( 1.0,  1.0,  1.0),
    vec3( 1.0,  1.0,  1.0), vec3( 1.0,  1.0, -1.0), vec3(-1.0,  1.0, -1.0)
);

void main() {
    vec3 pos = cubeVertices[gl_VertexIndex];
    outUv = pos;

    // Remove translation: skybox follows camera position
    vec3 camPos = sceneData.viewPos.xyz;
    vec4 worldPos = vec4(pos + camPos, 1.0);
    gl_Position = viewData.viewProjection * worldPos;

    // Force Z to far plane. Vulkan NDC Z is [0, 1], far = 1.0
    gl_Position.z = gl_Position.w;
}
