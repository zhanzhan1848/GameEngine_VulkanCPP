// Native frozen-profile TAA: actual raster matrices + paired depth/color history.
struct TAAGlobals {
    screenSize: vec4f,    // xy = (width, height), zw = (1/width, 1/height)
    invHistoryValid: f32, // 1.0 if history is uninitialized (first frame), 0.0 otherwise
    depthValid: u32,
    _pad1: u32,
    _pad2: u32,
    currInvViewProj:mat4x4<f32>,
    prevViewProj:mat4x4<f32>,
};

@group(0) @binding(0) var currentColorTex: texture_2d<f32>;
@group(0) @binding(1) var historyColorTex: texture_2d<f32>;
@group(0) @binding(2) var velocityTex: texture_2d<f32>;
@group(0) @binding(3) var outputTex: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var<uniform> globals: TAAGlobals;

@group(0) @binding(5) var currDepthTex:texture_depth_2d;
@group(0) @binding(6) var prevDepthTex:texture_2d<f32>;

// Manual bilinear via 4 textureLoads — bypasses textureSampleLevel, which
// silently fails WASM Dawn's uniformity analysis for compute shaders with
// non-uniform coords in non-uniform control flow. The failure manifests as
// EventManager::ProcessEvents OOB crash at end-of-frame because the pipeline
// ends up invalid and DawnShader::Initialize skips compile error polling on
// WASM, masking the WGSL compile error.
fn sampleBilinear(tex: texture_2d<f32>, uv: vec2f, dims: vec2u) -> vec3f {
    let coord = uv * vec2f(dims) - 0.5;
    let base = vec2i(i32(floor(coord.x)), i32(floor(coord.y)));
    let frac_ = fract(coord);
    let maxCoord = vec2i(i32(dims.x) - 1, i32(dims.y) - 1);

    let c00 = clamp(base, vec2i(0), maxCoord);
    let c10 = clamp(base + vec2i(1, 0), vec2i(0), maxCoord);
    let c01 = clamp(base + vec2i(0, 1), vec2i(0), maxCoord);
    let c11 = clamp(base + vec2i(1, 1), vec2i(0), maxCoord);

    let h00 = textureLoad(tex, vec2u(c00), 0).rgb;
    let h10 = textureLoad(tex, vec2u(c10), 0).rgb;
    let h01 = textureLoad(tex, vec2u(c01), 0).rgb;
    let h11 = textureLoad(tex, vec2u(c11), 0).rgb;

    let fx = clamp(frac_.x, 0.0, 1.0);
    let fy = clamp(frac_.y, 0.0, 1.0);

    return h00 * (1.0 - fx) * (1.0 - fy)
         + h10 * fx * (1.0 - fy)
         + h01 * (1.0 - fx) * fy
         + h11 * fx * fy;
}

// RGB <-> YCoCg (in-place color space for variance clipping — better chroma separation than YCbCr for natural scenes).
fn rgb_to_ycocg(c: vec3f) -> vec3f {
    let y  = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    let co = 0.5 * c.r - 0.5 * c.b;
    let cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return vec3f(y, co, cg);
}

fn ycocg_to_rgb(c: vec3f) -> vec3f {
    let y = c.x;
    let co = c.y;
    let cg = c.z;
    let tmp = y - cg;
    return vec3f(tmp + co, y + cg, tmp - co);
}

// Clip history to neighborhood AABB (in YCoCg space) using Karis-style clamp.
fn clip_to_aabb(aabb_min: vec3f, aabb_max: vec3f, p: vec3f) -> vec3f {
    let center = 0.5 * (aabb_min + aabb_max);
    let extents = 0.5 * (aabb_max - aabb_min);
    let v = p - center;
    let unit = v / max(extents, vec3f(0.0001));
    let abs_unit = abs(unit);
    let max_comp = max(abs_unit.x, max(abs_unit.y, abs_unit.z));
    if (max_comp <= 1.0) {
        return p; // inside AABB
    }
    return center + v / max_comp;
}

@compute @workgroup_size(8, 8, 1)
fn measured_taa(@builtin(global_invocation_id) gid: vec3u) {
    let dims = vec2u(globals.screenSize.xy);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = gid.xy;
    let currColor = textureLoad(currentColorTex, pixel, 0).rgb;
    let velocity = textureLoad(velocityTex, pixel, 0).xy;

    // Velocity is in clip-space NDC (Y up). Convert to UV-space delta (Y down for texture).
    // velocity_uv = (vel.x * 0.5, -vel.y * 0.5)
    // historyUV = currUV - velocity_uv
    let currUV = (vec2f(pixel) + 0.5) * globals.screenSize.zw;
    var historyUV = currUV - vec2f(velocity.x, -velocity.y) * 0.5;
    var disoccluded=false;
    if(globals.depthValid>0u){
        let depth=textureLoad(currDepthTex,vec2i(pixel),0);
        let ndc=vec3f(currUV.x*2.0-1.0,1.0-currUV.y*2.0,depth);
        let worldH=globals.currInvViewProj*vec4f(ndc,1);let world=worldH.xyz/max(worldH.w,1e-6);
        let previous=globals.prevViewProj*vec4f(world,1);let prevNDC=previous.xy/previous.w;let expectedDepth=previous.z/previous.w;
        historyUV=vec2f(prevNDC.x*.5+.5,.5-prevNDC.y*.5);
        let prevDims=vec2i(textureDimensions(prevDepthTex));let prevPixel=vec2i(historyUV*vec2f(prevDims));var closest=1.0;
        for(var dy=-1;dy<=1;dy++){for(var dx=-1;dx<=1;dx++){
            let samplePixel=clamp(prevPixel+vec2i(dx,dy),vec2i(0),prevDims-1);closest=min(closest,textureLoad(prevDepthTex,samplePixel,0).r);
        }}
        disoccluded=expectedDepth>closest+max(closest*.1,.002);
    }

    // Out-of-bounds history: keep current frame only
    var historyColor = currColor;
    if (globals.invHistoryValid < 0.5 &&
        historyUV.x >= 0.0 && historyUV.x <= 1.0 &&
        historyUV.y >= 0.0 && historyUV.y <= 1.0) {
        let histDims = vec2u(textureDimensions(historyColorTex, 0));
        historyColor = sampleBilinear(historyColorTex, historyUV, histDims);
    }

    // 3x3 neighborhood of current color (for variance clipping)
    var neighborMin = currColor;
    var neighborMax = currColor;
    for (var dy: i32 = -1; dy <= 1; dy++) {
        for (var dx: i32 = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) { continue; }
            let npixel = vec2i(i32(pixel.x) + dx, i32(pixel.y) + dy);
            if (npixel.x < 0 || npixel.x >= i32(dims.x) ||
                npixel.y < 0 || npixel.y >= i32(dims.y)) { continue; }
            let ncolor = textureLoad(currentColorTex, vec2u(npixel), 0).rgb;
            neighborMin = min(neighborMin, ncolor);
            neighborMax = max(neighborMax, ncolor);
        }
    }

    // Convert AABB to YCoCg space, clip history
    let aabbMinYC = rgb_to_ycocg(neighborMin);
    let aabbMaxYC = rgb_to_ycocg(neighborMax);
    let center=.5*(aabbMinYC+aabbMaxYC);let extents=.5*(aabbMaxYC-aabbMinYC)*1.25;
    let historyYC = clip_to_aabb(center-extents, center+extents, rgb_to_ycocg(historyColor));
    let historyClipped = ycocg_to_rgb(historyYC);

    // Blend — lower alpha for stability (slower convergence, less ghosting)
    // First frame: force alpha=1.0 to bypass uninitialized history.
    var alpha = 0.1;
    if (globals.invHistoryValid > 0.5 || disoccluded) {
        alpha = 1.0;
    }
    let result = mix(historyClipped, currColor, alpha);

    textureStore(outputTex, pixel, vec4f(result, 1.0));
}
