// NeuralCapture.metal — M1 神经降噪训练数据采集（Metal 侧，双入口）。
//
// GLSL 孪生：Engine/Graphics/Vulkan/shaders/Neural/NeuralCaptureUpdate.comp
//                                                  /NeuralCapturePack.comp
// 规格：输入 = SPGI gather 输出（每帧 jitter 的 1spp 估计量）；Welford
// 在线均值/M2（fp16 安全，量级 O(1)）；ping-pong sampled→writeonly，
// 不用 read-write storage image。pack 把统计 + depth/normal/noisy 打成
// 5 张 rgba32f 平面纹理，由 CPU 侧逐张 CopyTextureToBuffer 落盘。

#include <metal_stdlib>
using namespace metal;

struct NeuralCaptureCB {
    float4 res_frame;   // x=w y=h z=frame_in_take(0-based) w=frames_total
    float4 misc;        // x=near y=far z=take_idx w=reserved
};

// 更新：Welford 累积一帧。
kernel void neural_capture_update(
    texture2d<float>             gi_noisy     [[texture(0)]],
    texture2d<float>             mean_prev    [[texture(1)]],
    texture2d<float>             m2_prev      [[texture(2)]],
    texture2d<float, access::write> mean_dst  [[texture(3)]],
    texture2d<float, access::write> m2_dst    [[texture(4)]],
    texture2d<float, access::write> noisy_out [[texture(5)]],
    constant NeuralCaptureCB&    cb           [[buffer(0)]],
    uint2                        gid          [[thread_position_in_grid]])
{
    if (gid.x >= uint(cb.res_frame.x) || gid.y >= uint(cb.res_frame.y)) return;

    float3 x = gi_noisy.read(gid).rgb;

    // 噪声样本取第 8 帧（跳过 SPGI 时序历史冷启动），不影响 Welford 统计。
    if (fabs(cb.res_frame.z - 8.0f) < 0.5f) {
        noisy_out.write(float4(x, 1.0f), gid);
    }

    if (cb.res_frame.z < 0.5f) {
        // 首帧：mean = x，M2 = 0。
        mean_dst.write(float4(x, 1.0f), gid);
        m2_dst.write(float4(0.0f), gid);
        return;
    }

    float np1 = cb.res_frame.z + 1.0f;
    float3 mean = mean_prev.read(gid).rgb;
    float3 m2   = m2_prev.read(gid).rgb;

    float3 delta   = x - mean;
    float3 meanNew = mean + delta / np1;
    float3 delta2  = x - meanNew;
    float3 m2New   = m2 + delta * delta2;

    mean_dst.write(float4(meanNew, np1), gid);
    m2_dst.write(float4(m2New, 0.0f), gid);
}

// 收尾：统计 → 5 张 rgba32f 导出平面（深度原样导出，线性化在 python 侧）。
kernel void neural_capture_pack(
    texture2d<float>                in_mean    [[texture(0)]],
    texture2d<float>                in_m2      [[texture(1)]],
    texture2d<float>                in_noisy   [[texture(2)]],
    depth2d<float, access::read>    in_depth   [[texture(3)]],   // depth 纹理必须 depth2d
    texture2d<float>                in_normal  [[texture(4)]],
    texture2d<float, access::write> out_mean   [[texture(5)]],
    texture2d<float, access::write> out_var    [[texture(6)]],
    texture2d<float, access::write> out_depth  [[texture(7)]],
    texture2d<float, access::write> out_normal [[texture(8)]],
    texture2d<float, access::write> out_noisy  [[texture(9)]],
    constant NeuralCaptureCB&       cb         [[buffer(0)]],
    uint2                           gid        [[thread_position_in_grid]])
{
    if (gid.x >= uint(cb.res_frame.x) || gid.y >= uint(cb.res_frame.y)) return;

    float4 mean = in_mean.read(gid);
    float3 m2   = in_m2.read(gid).rgb;
    float n = max(cb.res_frame.w, 1.0f);
    float3 var = max(m2 / n - mean.rgb * mean.rgb, float3(0.0f));

    float3 noisy = in_noisy.read(gid).rgb;
    float depth  = in_depth.read(gid);
    float3 nrm   = clamp(in_normal.read(gid).xyz * 2.0f - 1.0f, -1.0f, 1.0f);

    out_mean.write(float4(mean.rgb, n), gid);
    out_var.write(float4(var, 0.0f), gid);
    out_depth.write(float4(depth, 0.0f, 0.0f, 0.0f), gid);
    out_normal.write(float4(nrm, 0.0f), gid);
    out_noisy.write(float4(noisy, 0.0f), gid);
}
