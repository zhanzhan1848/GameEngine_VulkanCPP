// ScreenProbeTraceRays.wgsl — Dawn port of ScreenProbeTraceRays.comp (Vulkan GLSL)
//
// Each thread traces one ray from one probe through the GlobalSDF volume using
// Fibonacci sphere sampling. On hit, samples prev-frame lit color (or surface
// cache atlas for near hits) as radiance. On miss, emits DDGI probe-grid SH
// irradiance (T2.2) or a sky-ambient color.
//
// ThreadGroupSize: (64, 1, 1), 1D dispatch — matches ScreenProbeGIPass.cpp
// (pipeDesc.threadGroupSize = {64, 1, 1}) and the .comp layout declaration.
//
// === Binding layout (flat Vulkan numbering, textures + buffers share the
//     same number space — WebGPU has no Metal-style overlapping namespaces) ===
//
//   binding 0  texture_3d<f32>         sdf0                    (textureLoad nearest)
//   binding 1  texture_3d<f32>         sdf1
//   binding 2  texture_3d<f32>         sdf2
//   binding 3  texture_2d<f32>         prevFrameColor          (manual bilinear)
//   binding 4  texture_2d<f32>         surface_cache_lighting  (may be dummy 1x1)
//   binding 5  storage, read           probePositions          (array<vec4<f32>>)
//   binding 6  storage, read           probeNormals            (array<vec4<f32>>)
//   binding 7  storage, read_write     probeRadiance           (gridW*gridH*raysPerProbe)
//   binding 8  uniform                 ScreenProbeGlobalData   (ends at ddgi_fb[2],
//                                                              per the .comp std140 block)
//   binding 9  storage, read           cards                   (array<SurfaceCacheCard>)
//   binding 10 storage, read           cardLookup              (declared for layout
//                                                              stability; unused here)
//   binding 11 storage, read           sc_grid_offsets         (array<u32>)
//   binding 12 storage, read           sc_grid_cards           (array<u32>)
//   binding 13 storage, read           ddgi_irradiance         (array<vec4<f32>>,
//                                                              SH3 @ 16B stride)

// ============================================================================
// Constants
// ============================================================================

const PI_VAL: f32       = 3.14159265358979323846;
const GOLDEN_RATIO: f32 = 1.618033988749895;
// Was 4 — too few for sphere tracing. Most rays failed to converge and reported
// "miss", producing extremely dim SPGI output. 32 gives rays enough iterations.
const MAX_SDF_STEPS: u32 = 32u;

// ============================================================================
// Struct definitions
// ============================================================================

// ScreenProbeGlobalData UBO — mirrors the .comp std140 block field-for-field
// (and the C++ struct at ScreenProbeGIPass.h:52). sdf_resolutions is uvec4 in
// the GLSL block — vec4<u32> here (identical 16B layout; reads raw uint bits).
struct ScreenProbeGlobalData {
    view_projection:      mat4x4<f32>,          //   0
    inv_view_projection:  mat4x4<f32>,          //  64
    camera_position:      vec4<f32>,            // 128
    grid_params:          vec4<f32>,            // 144: x=gridW, y=gridH, z=downsample, w=raysPerProbe
    trace_params:         vec4<f32>,            // 160: x=maxRayDist, y=gatherRadius, z=renderW, w=renderH
    sdf_origins:          array<vec4<f32>, 3>,  // 176
    sdf_voxel_sizes:      array<vec4<f32>, 3>,  // 224
    sdf_extents:          array<vec4<f32>, 3>,  // 272
    sdf_resolutions:      vec4<u32>,            // 320: xyz=res0/1/2, w=cascadeCount
    surface_cache_params: vec4<f32>,            // 336: x=atlasSize, y=cardCount, z=available, w=unused
    ddgi_fb:              array<vec4<f32>, 2>,  // 352: T2.2: [0]=origin.xyz+spacing, [1]=counts.xyz+enabled
};

// Matches C++ SurfaceCacheCard (64 bytes) — inlined from SurfaceCacheData.glsl.
// std430 layout: two vec4 (16B each) + 8 packed u32 = 64B, stride multiple of 16.
struct SurfaceCacheCard {
    center:           vec4<f32>,  // xyz = center, w unused
    extent:           vec4<f32>,  // xyz = half-extents, w unused
    axis_direction:   u32,        // low byte = axis, high byte = direction
    resolution:       u32,
    atlas_offset_x:   u32,
    atlas_offset_y:   u32,
    mesh_instance_id: u32,
    _pad1:            u32,
    _pad2:            u32,
    _pad3:            u32,
};

// Matches C++ SurfaceCacheCardLookup (48 bytes) — inlined from SurfaceCacheData.glsl.
struct SurfaceCacheCardLookup {
    aabb_min:   vec4<f32>,  // xyz = min, w unused
    aabb_max:   vec4<f32>,  // xyz = max, w unused
    card_start: u32,
    card_count: u32,
    _pad0:      u32,
    _pad1:      u32,
};

// Uniform card grid decode (T1.3) — function-value struct from
// SurfaceCacheData.glsl (never bound to a buffer; layout irrelevant).
struct SCGridParams {
    grid_min:   vec3<f32>,
    grid_max:   vec3<f32>,
    dims:       vec3<u32>,
    cell_size:  f32,
    card_count: u32,
};

// SDFHitResult — ported from SDFTraceCommon.metal via the .comp.
struct SDFHitResult {
    hit:      bool,
    position: vec3<f32>,
    distance: f32,
};

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var sdf0: texture_3d<f32>;
@group(0) @binding(1) var sdf1: texture_3d<f32>;
@group(0) @binding(2) var sdf2: texture_3d<f32>;
@group(0) @binding(3) var prevFrameColor: texture_2d<f32>;
@group(0) @binding(4) var surface_cache_lighting: texture_2d<f32>;

@group(0) @binding(5) var<storage, read> probePositions: array<vec4<f32>>;
@group(0) @binding(6) var<storage, read> probeNormals: array<vec4<f32>>;
@group(0) @binding(7) var<storage, read_write> probeRadiance: array<vec4<f32>>;

@group(0) @binding(8) var<uniform> globalData: ScreenProbeGlobalData;

@group(0) @binding(9) var<storage, read> cards: array<SurfaceCacheCard>;
@group(0) @binding(10) var<storage, read> cardLookup: array<SurfaceCacheCardLookup>;

// T1.3: uniform card grid — O(1) card lookup replaces the 32-card linear scan.
@group(0) @binding(11) var<storage, read> sc_grid_offsets: array<u32>;
@group(0) @binding(12) var<storage, read> sc_grid_cards: array<u32>;

// T2.2: DDGI probe irradiance (SH3 coefficients, 16B-stride elements) for miss fallback
@group(0) @binding(13) var<storage, read> ddgi_irradiance: array<vec4<f32>>;

// ============================================================================
// SurfaceCacheData helpers (inlined, translated from GLSL)
// ============================================================================

// Find atlas UV for a world position given a card. Returns true if inside.
fn worldToCardUV(world_pos: vec3<f32>, card: SurfaceCacheCard,
                 out_uv: ptr<function, vec2<f32>>) -> bool {
    let local = world_pos - card.center.xyz;
    var u: f32;
    var v: f32;
    var depth: f32;

    let axis: u32 = card.axis_direction & 0xFFu;
    let dir: u32 = (card.axis_direction >> 8u) & 0xFFu;

    if (axis == 0u) {
        depth = local.x * select(-1.0, 1.0, dir > 0u);
        u = (local.y / (card.extent.y * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else if (axis == 1u) {
        depth = local.y * select(-1.0, 1.0, dir > 0u);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else {
        depth = local.z * select(-1.0, 1.0, dir > 0u);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.y / (card.extent.y * 2.0)) + 0.5;
    }

    if (depth < 0.0 || u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) {
        return false;
    }

    (*out_uv).x = f32(card.atlas_offset_x) + u * f32(card.resolution);
    (*out_uv).y = f32(card.atlas_offset_y) + v * f32(card.resolution);
    return true;
}

// Decode the 3 header uvec4s (words 0..11 of grid_offsets).
fn scGridDecode(h0: vec4<u32>, h1: vec4<u32>, h2: vec4<u32>) -> SCGridParams {
    var gp: SCGridParams;
    gp.grid_min = bitcast<vec3<f32>>(h0.xyz);
    gp.card_count = h0.w;
    gp.grid_max = bitcast<vec3<f32>>(h1.xyz);
    gp.cell_size = bitcast<f32>(h1.w);
    gp.dims = h2.xyz;
    return gp;
}

// World position → linear cell index; 0xFFFFFFFF when outside the grid.
fn scGridCellIndex(gp: SCGridParams, p: vec3<f32>) -> u32 {
    let g = (p - gp.grid_min) / gp.cell_size;
    if (g.x < 0.0 || g.y < 0.0 || g.z < 0.0) {
        return 0xFFFFFFFFu;
    }
    let c = vec3<u32>(g);
    if (c.x >= gp.dims.x || c.y >= gp.dims.y || c.z >= gp.dims.z) {
        return 0xFFFFFFFFu;
    }
    return c.x + gp.dims.x * (c.y + gp.dims.y * c.z);
}

// ============================================================================
// SDF cascade sampling (inlined from SDFTraceCommon.metal via the .comp).
// Metal uses linear-filtered texture3D with clamp_to_edge. Vulkan port (and
// this Dawn port) uses nearest textureLoad (porting rule 17: nearest
// acceptable for SDF tracing). Cascade convention matches Metal:
// uvw = (worldPos - origin) / extent, with origin = corner and extent = full
// box size, so [0,1] spans the whole volume.
// ============================================================================

fn isInsideCascade(pos: vec3<f32>, origin: vec3<f32>, extent: vec3<f32>) -> bool {
    let local = pos - origin;
    return local.x >= 0.0 && local.x < extent.x &&
           local.y >= 0.0 && local.y < extent.y &&
           local.z >= 0.0 && local.z < extent.z;
}

fn sampleSDFCascade(sdfTexture: texture_3d<f32>, worldPos: vec3<f32>,
                    cascadeOrigin: vec3<f32>, cascadeExtent: vec3<f32>) -> f32 {
    let uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    if (uvw.x < 0.0 || uvw.x > 1.0 ||
        uvw.y < 0.0 || uvw.y > 1.0 ||
        uvw.z < 0.0 || uvw.z > 1.0) {
        return 1.0e10;
    }
    let size = vec3<i32>(textureDimensions(sdfTexture));
    let coord = vec3<i32>(clamp(uvw, vec3<f32>(0.0), vec3<f32>(1.0)) * vec3<f32>(size));
    return textureLoad(sdfTexture, coord, 0).r;
}

// else-if cascade selection — 1 texture3D read per call (finest first).
fn sampleBestSDF(pos: vec3<f32>) -> f32 {
    if (globalData.sdf_resolutions.w > 0u &&
        isInsideCascade(pos, globalData.sdf_origins[0].xyz, globalData.sdf_extents[0].xyz)) {
        return sampleSDFCascade(sdf0, pos, globalData.sdf_origins[0].xyz, globalData.sdf_extents[0].xyz);
    } else if (globalData.sdf_resolutions.w > 1u &&
               isInsideCascade(pos, globalData.sdf_origins[1].xyz, globalData.sdf_extents[1].xyz)) {
        return sampleSDFCascade(sdf1, pos, globalData.sdf_origins[1].xyz, globalData.sdf_extents[1].xyz);
    } else if (globalData.sdf_resolutions.w > 2u &&
               isInsideCascade(pos, globalData.sdf_origins[2].xyz, globalData.sdf_extents[2].xyz)) {
        return sampleSDFCascade(sdf2, pos, globalData.sdf_origins[2].xyz, globalData.sdf_extents[2].xyz);
    }
    return 1.0e10;
}

// ============================================================================
// Sphere tracing through GlobalSDF (ported from TraceRays.metal::traceSDF)
// ============================================================================

fn traceSDF(origin: vec3<f32>, direction: vec3<f32>, maxDist: f32) -> SDFHitResult {
    var result: SDFHitResult;
    result.hit = false;
    result.position = origin;
    result.distance = 0.0;

    var pos = origin;
    var remainingDist = maxDist;

    let hitThreshold = globalData.sdf_voxel_sizes[0].x * 0.5;
    let minStep = globalData.sdf_voxel_sizes[0].x * 0.25;

    for (var stepIdx: u32 = 0u; stepIdx < MAX_SDF_STEPS; stepIdx++) {
        let d = sampleBestSDF(pos);

        if (d < hitThreshold) {
            result.hit = true;
            result.position = pos;
            result.distance = length(pos - origin);
            return result;
        }

        // Relaxed advancement: overstep when far, conservative when close
        var advance: f32;
        if (d > hitThreshold * 4.0) {
            advance = d * 1.2;
        } else {
            advance = max(d, minStep);
        }
        if (advance > remainingDist) {
            break;
        }

        pos = pos + direction * advance;
        remainingDist = remainingDist - advance;
    }

    result.distance = length(pos - origin);
    return result;
}

// ============================================================================
// Fibonacci sphere direction generation + normal alignment
// ============================================================================

fn fibonacciDirection(rayIndex: u32, totalRays: u32) -> vec3<f32> {
    let theta = 2.0 * PI_VAL * f32(rayIndex) / GOLDEN_RATIO;
    let phi = acos(1.0 - 2.0 * (f32(rayIndex) + 0.5) / f32(totalRays));
    return vec3<f32>(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));
}

fn orientAroundNormal(dir: vec3<f32>, normal: vec3<f32>) -> vec3<f32> {
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(normal.y) < 0.999);
    let tangent = normalize(cross(up, normal));
    let bitangent = cross(normal, tangent);
    return normalize(tangent * dir.x + bitangent * dir.y + normal * dir.z);
}

// ============================================================================
// Manual bilinear sample of a 2D texture (porting rule 17 alternative).
// Replicates Metal linear sampler (clamp_to_edge).
// ============================================================================

fn sampleBilinearRGB(tex: texture_2d<f32>, uv: vec2<f32>) -> vec3<f32> {
    let size = vec2<f32>(textureDimensions(tex));
    let px = uv * size - vec2<f32>(0.5);
    let p0 = vec2<i32>(floor(px));
    let f = fract(px);

    let maxi = vec2<i32>(size) - vec2<i32>(1);
    let c00 = clamp(p0,                   vec2<i32>(0), maxi);
    let c10 = clamp(p0 + vec2<i32>(1, 0), vec2<i32>(0), maxi);
    let c01 = clamp(p0 + vec2<i32>(0, 1), vec2<i32>(0), maxi);
    let c11 = clamp(p0 + vec2<i32>(1, 1), vec2<i32>(0), maxi);

    let s00 = textureLoad(tex, c00, 0).rgb;
    let s10 = textureLoad(tex, c10, 0).rgb;
    let s01 = textureLoad(tex, c01, 0).rgb;
    let s11 = textureLoad(tex, c11, 0).rgb;

    let a = mix(s00, s10, f.x);
    let b = mix(s01, s11, f.x);
    return mix(a, b, f.y);
}

// ============================================================================
// Sample prev frame color at a world position
// (ported from TraceRays.metal::samplePrevFrameColor)
// ============================================================================

fn samplePrevFrameColor(worldPos: vec3<f32>) -> vec3<f32> {
    // Project to screen space
    let clipPos = globalData.view_projection * vec4<f32>(worldPos, 1.0);
    if (clipPos.w <= 0.0) {
        return vec3<f32>(0.0);
    }

    let ndc = clipPos.xy / clipPos.w;
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 1.0 - ndc.y * 0.5 - 0.5);

    // Bounds check
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return vec3<f32>(0.0);
    }

    return sampleBilinearRGB(prevFrameColor, uv);
}

// ============================================================================
// Main Kernel: screen_probe_trace_rays
// ============================================================================

@compute @workgroup_size(64, 1, 1)
fn screen_probe_trace_rays(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let global_id: u32 = gid_vec.x;

    let gridW: u32 = u32(globalData.grid_params.x);
    let gridH: u32 = u32(globalData.grid_params.y);
    let raysPerProbe: u32 = u32(globalData.grid_params.w);
    let totalRays: u32 = gridW * gridH * raysPerProbe;

    if (global_id >= totalRays) {
        return;
    }

    let probeIdx: u32 = global_id / raysPerProbe;
    let rayIdx: u32 = global_id % raysPerProbe;

    // Read probe position and normal
    let posData: vec4<f32> = probePositions[probeIdx];
    let normData: vec4<f32> = probeNormals[probeIdx];

    // Check if probe is active (w > 0)
    if (posData.w <= 0.0) {
        probeRadiance[global_id] = vec4<f32>(0.0, 0.0, 0.0, -1.0); // -1 = inactive
        return;
    }

    let probePos = posData.xyz;
    let probeNormal = normalize(normData.xyz);

    // Generate ray direction (Fibonacci + normal alignment)
    var rayDir = fibonacciDirection(rayIdx, raysPerProbe);
    rayDir = orientAroundNormal(rayDir, probeNormal);

    // Offset origin slightly to avoid self-intersection
    let origin = probePos + probeNormal * globalData.sdf_voxel_sizes[0].x * 2.0;

    // Trace ray through GlobalSDF
    let maxDist = globalData.trace_params.x;
    let hit = traceSDF(origin, rayDir, maxDist);

    var radiance = vec3<f32>(0.0);
    var cosTheta: f32 = 0.0; // cosine weight for irradiance integration

    if (hit.hit) {
        // Try surface cache sampling for near hits (< 2 world units)
        var usedSurfaceCache = false;
        let nearDistance = 2.0;

        if (hit.distance < nearDistance && globalData.surface_cache_params.z > 0.5) {
            // T1.3: grid lookup — candidates from the hit position's cell
            // (≤ kMaxCardsPerGridCell entries) instead of scanning 32 cards.
            let atlasSize = globalData.surface_cache_params.x;

            let gh0 = vec4<u32>(sc_grid_offsets[0], sc_grid_offsets[1],
                                sc_grid_offsets[2], sc_grid_offsets[3]);
            let gh1 = vec4<u32>(sc_grid_offsets[4], sc_grid_offsets[5],
                                sc_grid_offsets[6], sc_grid_offsets[7]);
            let gh2 = vec4<u32>(sc_grid_offsets[8], sc_grid_offsets[9],
                                sc_grid_offsets[10], sc_grid_offsets[11]);
            let gp = scGridDecode(gh0, gh1, gh2);
            let cell = scGridCellIndex(gp, hit.position);
            if (cell != 0xFFFFFFFFu) {
                let begin = sc_grid_offsets[16u + cell];
                let end = sc_grid_offsets[17u + cell];
                for (var gi: u32 = begin; gi < end && !usedSurfaceCache; gi++) {
                    var atlas_uv: vec2<f32>;
                    if (worldToCardUV(hit.position, cards[sc_grid_cards[gi]], &atlas_uv)) {
                        let uv_norm = atlas_uv / vec2<f32>(atlasSize);
                        radiance = sampleBilinearRGB(surface_cache_lighting, uv_norm);
                        usedSurfaceCache = true;
                    }
                }
            }
        }

        // Fall back to prev frame color if surface cache not used or not available
        if (!usedSurfaceCache) {
            radiance = samplePrevFrameColor(hit.position);
        }

        // NaN/Inf guard, manual per-component (naga lacks isnan/isinf builtins):
        // NaN fails x == x; Inf gives x - x = NaN → != 0. Together equivalent
        // to the GLSL `any(isnan(radiance)) || any(isinf(radiance))` guard.
        let hasNaN = (radiance.x != radiance.x) ||
                     (radiance.y != radiance.y) ||
                     (radiance.z != radiance.z);
        let hasInf = (radiance.x - radiance.x != 0.0) ||
                     (radiance.y - radiance.y != 0.0) ||
                     (radiance.z - radiance.z != 0.0);
        if (hasNaN || hasInf) {
            radiance = vec3<f32>(0.0);
        }

        // Store cosine weight (applied during averaging)
        cosTheta = max(dot(probeNormal, rayDir), 0.0);
    } else {
        // Miss ray fallback chain (T2.2): DDGI probe grid → sky gradient.
        if (globalData.ddgi_fb[1].w > 0.5) {
            let org = globalData.ddgi_fb[0].xyz;
            let spacing = max(globalData.ddgi_fb[0].w, 0.01);
            let counts = vec3<i32>(globalData.ddgi_fb[1].xyz);
            var gc = vec3<i32>(floor((probePos - org) / spacing));
            gc = clamp(gc, vec3<i32>(0), counts - vec3<i32>(1));
            let pidx = u32(gc.x) + u32(counts.x) * (u32(gc.y) + u32(counts.y) * u32(gc.z));
            // SH3 coefficients @ 16B stride (vec4 view of the DDGI buffer)
            let sh0 = ddgi_irradiance[pidx * 9u + 0u].xyz;
            let sh1 = ddgi_irradiance[pidx * 9u + 1u].xyz;
            let sh2 = ddgi_irradiance[pidx * 9u + 2u].xyz;
            let sh3 = ddgi_irradiance[pidx * 9u + 3u].xyz;
            radiance = max(vec3<f32>(0.0),
                0.282095 * sh0
                - 0.488603 * sh1 * rayDir.y
                + 0.488603 * sh2 * rayDir.z
                - 0.488603 * sh3 * rayDir.x);
            cosTheta = max(dot(probeNormal, rayDir), 0.0);
        } else {
            // Simple sky gradient: blue-ish at horizon, lighter at zenith
            let skyWeight = max(dot(probeNormal, rayDir), 0.0);
            if (skyWeight > 0.0) {
                let skyY = rayDir.y * 0.5 + 0.5;
                radiance = mix(vec3<f32>(0.15, 0.18, 0.25), vec3<f32>(0.4, 0.5, 0.65), skyY);
                cosTheta = skyWeight;
            }
        }
    }

    // Write output: xyz = radiance, w = cosine weight (>= 0 for both hit and sky miss)
    let outputWeight = cosTheta;
    probeRadiance[global_id] = vec4<f32>(radiance, outputWeight);
}
