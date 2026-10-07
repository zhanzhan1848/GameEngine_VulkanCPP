#version 450 core
// ProbeSphere.frag — step 1 color: per-probe stable hue from the instance id
// + sun diffuse. ZERO descriptors (same class as the working bring-up test).
// Probe LIVE state coloring (L0 energy / buried red from the irradiance and
// depth SSBOs) is step 2, added only after this renders.

// Sun hardcoded during bring-up (VS-only push constants).

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vClip;
layout(location = 3) flat in uint vProbe;
// 2026-09-17 审计 §8.3：重定位分类（2.0 = 无状态数据 → legacy 色相）。
layout(location = 4) flat in float vState;
// [P0/④] 着色模式（VS PC misc.x 直通）：2 = confidence、3 = variance 时
// 本 frag 从 moments（binding 1）读 DDGI 八面体深度统计渲染热图。
layout(location = 5) flat in float vMode;

layout(location = 0) out vec4 outColor;

// [④] DDGI 深度 moments（DDGIUpdateDepth 写入：probeIdx×128 floats =
// 8×8 mean[0..63] + var[64..127]，八面体 texel 与 UpdateDepth 同式）。
// 仅 mode 2/3 且模块绑定 moments 缓冲时有意义；state/radiance 模式忽略。
layout(std430, set = 0, binding = 1) readonly buffer MomentsSSBO {
    float moments[];
};

vec2 probeOctEncode(vec3 d) {
    float l1norm = abs(d.x) + abs(d.y) + abs(d.z);
    vec2 uv = d.xy / l1norm;
    if (d.z < 0.0) {
        vec2 s = mix(vec2(-1.0), vec2(1.0), vec2(greaterThanEqual(uv.xy, vec2(0.0))));
        uv = (1.0 - abs(uv.yx)) * s;
    }
    return uv * 0.5 + 0.5;
}

void main() {
    vec3 octN = normalize(vNormal);

    // [④ 2026-09-21] DDGI moments 热图——confidence/variance 模式的 DDGI 腿。
    // confidence = 1/(1+var)：八面体距离方差小 = 该方向表面距离测量可信；
    // variance = var/(mean²+ε)：相对距离方差（越黄越弥散/多表面混叠）。
    if (vMode > 1.5) {
        const uint RES = 8u;
        vec2 uv = probeOctEncode(octN);
        uvec2 texel = uvec2(clamp(uint(uv.x * float(RES)), 0u, RES - 1u),
                            clamp(uint(uv.y * float(RES)), 0u, RES - 1u));
        uint t = texel.y * RES + texel.x;
        float meanD = moments[vProbe * 128u + t];
        float varD = moments[vProbe * 128u + 64u + t];
        if (vMode < 2.5) {
            float conf = 1.0 / (1.0 + varD);
            vec3 heat = mix(vec3(0.85, 0.20, 0.15), vec3(0.20, 0.85, 0.30), clamp(conf, 0.0, 1.0));
            outColor = vec4(heat, 1.0);
        } else {
            float rel = clamp(varD / (meanD * meanD + 1e-3), 0.0, 1.0);
            vec3 heat = mix(vec3(0.20, 0.45, 0.85), vec3(0.95, 0.80, 0.20), rel);
            outColor = vec4(heat, 1.0);
        }
        return;
    }

    // 2026-09-17：状态色优先——绿=本格自由、黄=本格重定位、红=借格、灰=未
    // trace（与 DDGIOriginStats 口径一致）。2.0 哨兵 = 无状态数据，保持旧
    // 黄金比例色相。
    if (vState < 1.5) {
        vec3 stateColor;
        if (vState < 0.125)      stateColor = vec3(0.45, 0.45, 0.48); // untraced
        else if (vState < 0.625) stateColor = vec3(0.90, 0.15, 0.10); // borrowed
        else if (vState < 0.875) stateColor = vec3(0.95, 0.80, 0.10); // relocated
        else                     stateColor = vec3(0.15, 0.85, 0.25); // self free
        vec3 N = normalize(vNormal);
        // half-lambert：避免背光面片全黑（调试可读性优先）。
        float ndl = 0.5 + 0.5 * dot(N, normalize(vec3(0.4, 0.8, 0.3)));
        outColor = vec4(stateColor * (0.35 + 0.65 * ndl), 1.0);
        return;
    }
    // Stable per-probe hue (golden-ratio spacing around the wheel) so the
    // lattice reads as individual probes even without live data.
    // Per-probe stable hue (golden-ratio wheel) so the lattice reads as
    // individual probes.
    float h = fract(float(vProbe) * 0.6180339887);
    vec3 hue = clamp(vec3(1.5 - abs(h * 6.0 - 3.0) - 1.0,
                          1.5 - abs(h * 6.0 - 1.0) - 1.0,
                          1.5 - abs(h * 6.0 - 5.0) - 1.0),
                     vec3(0.25), vec3(1.0));
    vec3 N = normalize(vNormal);
    float ndl = 0.5 + 0.5 * dot(N, normalize(vec3(0.4, 0.8, 0.3)));
    float shade = 0.35 + 0.65 * ndl;
    outColor = vec4(hue * shade, 1.0);
}
