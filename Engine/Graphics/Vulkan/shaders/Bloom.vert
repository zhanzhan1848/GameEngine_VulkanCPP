#version 450 core

// Vulkan bright-pass vertex stage.  Keep the clip-space and UV convention of
// the WGSL source: Vulkan's viewport conversion requires flipping clip-space Y
// while the sampled HDR image remains in the existing fullscreen orientation.

layout(location = 0) out vec2 outUv;

void bloom_vs() {
    vec4 positions[3] = vec4[3](
        vec4(-1.0, -1.0, 0.0, 1.0),
        vec4( 3.0, -1.0, 0.0, 1.0),
        vec4(-1.0,  3.0, 0.0, 1.0)
    );
    vec2 uvs[3] = vec2[3](
        vec2(0.0, 1.0),
        vec2(2.0, 1.0),
        vec2(0.0, -1.0)
    );

    vec4 position = positions[gl_VertexIndex];
    gl_Position = vec4(position.x, -position.y, position.z, position.w);
    outUv = uvs[gl_VertexIndex];
}
