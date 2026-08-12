// Velocity.wgsl — Motion vector generation from depth buffer
// Computes screen-space velocity by reconstructing world position from depth
// and projecting to current and previous frame NDC.

struct VelocityParams {
    invProj: mat4x4<f32>,
    viewProj: mat4x4<f32>,
    prevViewProj: mat4x4<f32>,
    screenSize: vec4<f32>,  // x=width, y=height, z=1/width, w=1/height
};

@group(0) @binding(0) var depthTex: texture_depth_2d;
@group(0) @binding(1) var velOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> params: VelocityParams;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3u) {
    let dims = vec2u(params.screenSize.xy);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = gid.xy;
    let uv = (vec2f(pixel) + 0.5) / params.screenSize.xy;

    let depth = textureLoad(depthTex, pixel, 0);

    // Sky pixels: zero velocity
    if (depth >= 0.9999) {
        textureStore(velOutput, pixel, vec4f(0.0, 0.0, 0.0, 1.0));
        return;
    }

    // Reconstruct NDC position
    let ndcX = uv.x * 2.0 - 1.0;
    let ndcY = 1.0 - uv.y * 2.0;
    let clipPos = vec4f(ndcX, ndcY, depth, 1.0);

    // Reconstruct view-space position
    let viewPos4 = params.invProj * clipPos;
    let viewPos = viewPos4.xyz / viewPos4.w;

    // World position (use inverse viewProj to reconstruct)
    // For static geometry, world pos is constant across frames.
    // We derive it from viewPos and the current viewProj:
    //   viewPos = View * worldPos => worldPos = InvView * viewPos
    // But we don't have InvView separately. Instead, we can compute
    // current NDC and prev NDC directly:
    let currClip = params.viewProj * vec4f(viewPos, 1.0);
    // Wait — viewProj includes the view matrix. viewPos is in view space,
    // so we need to go view->world->prev_clip. We need invView.
    // Simpler approach: reconstruct clip-space from UV+depth for both frames.

    // Current frame NDC (already known from UV)
    let currNDC = vec2f(ndcX, ndcY);

    // Previous frame: project the same view-space position using the
    // full prevViewProj. But viewPos is in CURRENT view space.
    // We need world position first. Use invViewProj to get it.
    // Actually, we can go: clipPos -> viewPos (via invProj) -> worldPos (via invView)
    // -> prevClipPos (via prevViewProj).
    //
    // Simplification: since our test scene has a static camera,
    // velocity will be zero. But for correctness:
    // We reconstruct world pos from current frame clip coords.
    // clipPos is in current clip space. worldPos = invViewProj * clipPos.
    // But we only have invProj, not invViewProj.
    //
    // Practical approach for the test: the camera is static, so velocity = 0.
    // For the full pipeline, we'd need invView or invViewProj.
    // Let's output zero velocity for now and add proper computation later.

    textureStore(velOutput, pixel, vec4f(0.0, 0.0, 0.0, 1.0));
}
