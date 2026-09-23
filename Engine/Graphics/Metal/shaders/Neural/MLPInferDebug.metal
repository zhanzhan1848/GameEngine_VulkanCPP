// MLPInferDebug.metal — M0 神经渲染 debug 推理 kernel（Metal 侧）。
//
// GLSL 孪生：Engine/Graphics/Vulkan/shaders/Neural/MLPInfer.comp
// CPU 参考拷贝：Engine/Graphics/Neural/NeuralWeights.cpp
// 训练/规格权威：ContentTools/neural/train_image_fit.py
//
// === PRIMAL NEURAL SPEC v1（四处同步，勿单处修改） ===
// 见 train_image_fit.py 顶部注释。要点：
//   hash grid: L=16 级 × F=2 特征，每级 entries = 2 的幂，table 布局
//     [level][entry][F]，线性索引 (level*T + idx)*F + comp；
//   h = l*0x9E3779B9 ^ i*0x85EBCA6B ^ j*0xC2B2AE35; h *= 0x27D4EB2F; h ^= h>>15;
//   网格坐标全 float32：q = uv*res, i0=min(floor(q),res-1), f=q-floor(q),
//   i1=min(i0+1,res-1)，双线性插值特征；raw (u,v) 追加在尾部；
//   MLP 逐 i 升序累加（acc = b; acc += x[i]*W[o][i]），与 CPU/Python 同序。

#include <metal_stdlib>
using namespace metal;

// 与 C++ NeuralDebugCB（NeuralDebugModule.cpp）逐字节镜像（std140 / MSL 对齐一致）。
struct NeuralDebugCB {
    float4 res_mode;       // x=outW y=outH z=split(1)/NN-full(0) w=layer_count
    float4 enc;            // x=levels y=features z=log2(T) w=table_count(f32 个数)
    float4 level_res[4];   // 每级分辨率（f32 精确整数，< 2^24）
    float4 layers[8];      // x=in_dim y=out_dim z=W 偏移(f32 idx) w=B 偏移
};

constant uint kHashSalt   = 0x9E3779B9u;
constant uint kHashPrimeI = 0x85EBCA6Bu;
constant uint kHashPrimeJ = 0xC2B2AE35u;
constant uint kHashMix    = 0x27D4EB2Fu;

inline uint neural_hash3(uint i, uint j, uint l) {
    uint h = l * kHashSalt;
    h ^= i * kHashPrimeI;
    h ^= j * kHashPrimeJ;
    h *= kHashMix;
    h ^= h >> 15u;
    return h;
}

// GT 参考函数（训练目标；与 Python gt_image / GLSL 孪生逐 op 一致）。
inline float3 neural_gt_image(float u, float v) {
    float3 sky_lo = float3(0.06, 0.12, 0.38);
    float3 sky_hi = float3(1.00, 0.50, 0.18);
    float3 sky = sky_lo + (sky_hi - sky_lo) * smoothstep(0.12, 0.88, v);
    float d = sqrt((u - 0.62) * (u - 0.62) + (v - 0.40) * (v - 0.40));
    float sun = exp(-d * 14.0) * 1.6;
    float rh = 0.30 + 0.08 * sin(u * 9.0) + 0.05 * sin(u * 23.0 + 1.7)
             + 0.02 * sin(u * 57.0 + 0.5);
    float ridge = smoothstep(rh + 0.004, rh - 0.004, v);
    float3 mtn = float3(0.05, 0.07, 0.10)
               + (float3(0.16, 0.13, 0.17) - float3(0.05, 0.07, 0.10)) * smoothstep(0.0, 0.25, v);
    float3 col = sky * (1.0 - ridge) + mtn * ridge;
    col += sun * (1.0 - ridge) + sun * 0.25 * ridge;
    return clamp(col, 0.0, 1.0);
}

inline void neural_hash_encode(device const float* pw, constant NeuralDebugCB& cb,
                               float u, float v, float feat[64]) {
    const uint levels = uint(cb.enc.x);
    const uint table_count = uint(cb.enc.w);
    for (uint l = 0; l < levels; ++l) {
        const float res = cb.level_res[l >> 2][l & 3u];
        const float res_m1 = res - 1.0f;
        const float qx = u * res;
        const float qy = v * res;
        const float flx = floor(qx);
        const float fly = floor(qy);
        const float i0 = min(flx, res_m1);
        const float j0 = min(fly, res_m1);
        const float fx = qx - flx;
        const float fy = qy - fly;
        const float i1 = min(i0 + 1.0f, res_m1);
        const float j1 = min(j0 + 1.0f, res_m1);
        const uint per_level = table_count / levels;
        const uint mask = (1u << uint(cb.enc.z)) - 1u;   // entries = 2^log2T
        const uint base = l * per_level;
        const uint h00 = neural_hash3(uint(i0), uint(j0), l) & mask;
        const uint h10 = neural_hash3(uint(i1), uint(j0), l) & mask;
        const uint h01 = neural_hash3(uint(i0), uint(j1), l) & mask;
        const uint h11 = neural_hash3(uint(i1), uint(j1), l) & mask;
        const float w00 = (1.0f - fx) * (1.0f - fy);
        const float w10 = fx * (1.0f - fy);
        const float w01 = (1.0f - fx) * fy;
        const float w11 = fx * fy;
        for (uint c = 0; c < 2u; ++c) {   // F = 2
            feat[l * 2u + c] =
                w00 * pw[base + h00 * 2u + c] + w10 * pw[base + h10 * 2u + c]
              + w01 * pw[base + h01 * 2u + c] + w11 * pw[base + h11 * 2u + c];
        }
    }
    feat[levels * 2u + 0] = u;
    feat[levels * 2u + 1] = v;
}

// Compute：每像素一次 hash-encode + MLP 前向。
// 左半屏 = 网络重建，右半屏 = GT 直接求值（split 模式）。
kernel void neural_mlp_infer(
    texture2d<float, access::write> out_img   [[texture(0)]],
    device const float*               pw      [[buffer(0)]],
    constant NeuralDebugCB&           cb      [[buffer(1)]],
    uint2                             gid     [[thread_position_in_grid]])
{
    if (gid.x >= out_img.get_width() || gid.y >= out_img.get_height()) return;

    const float u = (float(gid.x) + 0.5f) / float(out_img.get_width());
    const float v = (float(gid.y) + 0.5f) / float(out_img.get_height());

    float arr0[64], arr1[64];
    thread float* src = arr0;
    thread float* dst = arr1;
    neural_hash_encode(pw, cb, u, v, src);

    const uint layer_count = uint(cb.res_mode.w);
    for (uint k = 0; k < layer_count; ++k) {
        const uint in_dim  = uint(cb.layers[k].x);
        const uint out_dim = uint(cb.layers[k].y);
        device const float* w = pw + uint(cb.layers[k].z);
        device const float* b = pw + uint(cb.layers[k].w);
        const float act_k = (k + 1u < layer_count) ? 0.0f : 2.0f;  // 隐层 relu，末层 sigmoid
        for (uint o = 0; o < out_dim; ++o) {
            device const float* row = w + o * in_dim;
            float acc = b[o];
            for (uint i = 0; i < in_dim; ++i) acc += src[i] * row[i];
            acc = (act_k < 0.5f) ? max(acc, 0.0f) : (1.0f / (1.0f + exp(-acc)));
            dst[o] = acc;
        }
        thread float* t = src; src = dst; dst = t;
    }

    const bool split = cb.res_mode.z > 0.5f;
    const bool left = gid.x < out_img.get_width() / 2u;
    float3 col = float3(src[0], src[1], src[2]);
    if (split && !left) col = neural_gt_image(u, v);
    out_img.write(float4(col, 1.0f), gid);
}

// ---------------------------------------------------------------------------
// 全屏三角形 blit（StorageImage → backbuffer）：tonemap + gamma + 中缝线。
// ---------------------------------------------------------------------------
struct NeuralDebugVtxOut {
    float4 pos [[position]];
    float2 uv;
};

vertex NeuralDebugVtxOut neural_debug_vs(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u));
    NeuralDebugVtxOut o;
    o.pos = float4(p * 2.0f - 1.0f, 0.0, 1.0);
    o.uv = p;
    return o;
}

fragment float4 neural_debug_fs(
    NeuralDebugVtxOut in              [[stage_in]],
    texture2d<float>  nn_tex          [[texture(0)]])
{
    const uint w = nn_tex.get_width();
    const uint h = nn_tex.get_height();
    uint2 tc = uint2(in.uv * float2(w, h));
    tc = min(tc, uint2(w - 1u, h - 1u));
    float3 c = nn_tex.read(tc).rgb;
    c = 1.0 - exp(-c * 1.6);            // tonemap
    c = pow(c, float3(1.0 / 2.2));      // gamma
    if (abs(in.uv.x - 0.5) < 0.0015) c = float3(0.0);
    return float4(c, 1.0);
}
