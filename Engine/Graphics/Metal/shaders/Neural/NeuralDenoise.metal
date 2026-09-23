// NeuralDenoise.metal — M1 降噪器推理 kernel（Metal 侧，双入口）。
//
// GLSL 孪生：Engine/Graphics/Vulkan/shaders/Neural/NeuralDenoisePack.comp
//                                                  /NeuralConv.comp
// 数值语义与 CPU 参考（NeuralWeights.cpp ReferenceForwardConv）逐位对齐：
// zero-pad、c → ky → kx 累加、隐层 relu、末层 expm1 + log 域残差。

#include <metal_stdlib>
using namespace metal;

struct NeuralConvCB {
    float4 layer0;   // x=in_c y=out_c z=w_offset(f32 idx) w=b_offset
    float4 layer1;   // x=kernel y=pad z=flags(bit0=expm1) w=net_residual
    float4 res;      // x=w y=h
};

struct NeuralPackCB {
    float4 res_misc;   // x=w y=h z=depth_norm_inv
};

// 输入打包：7ch → 2 张 rgba16f（T0=log1p(gi)+depth/16，T1=normal）。
kernel void neural_denoise_pack(
    texture2d<float>                in_gi     [[texture(0)]],
    depth2d<float, access::read>    in_depth  [[texture(1)]],
    texture2d<float>                in_normal [[texture(2)]],
    texture2d<float, access::write> out_t0    [[texture(3)]],
    texture2d<float, access::write> out_t1    [[texture(4)]],
    constant NeuralPackCB&          cb        [[buffer(0)]],
    uint2                           gid       [[thread_position_in_grid]])
{
    if (gid.x >= uint(cb.res_misc.x) || gid.y >= uint(cb.res_misc.y)) return;

    float3 gi = clamp(in_gi.read(gid).rgb, 0.0f, 27.0f);
    float3 glog = log(1.0f + gi);
    float d = clamp(in_depth.read(gid) * cb.res_misc.z, 0.0f, 1.0f);
    float3 nrm = clamp(in_normal.read(gid).xyz * 2.0f - 1.0f, -1.0f, 1.0f);

    // 通道布局与模型一致：ch0..2=log(gi) ch3..5=normal ch6=depth/16
    out_t0.write(float4(glog, nrm.x), gid);
    out_t1.write(float4(nrm.y, nrm.z, d, 0.0f), gid);
}

// 通用 3×3 conv 层（每层一次 dispatch）。邻域寄存器化：每输入纹理 3×3
// 只读一次，MAC 全走寄存器（与 GLSL 孪生同构）。
kernel void neural_conv(
    texture2d<float>                in0      [[texture(0)]],
    texture2d<float>                in1      [[texture(1)]],
    texture2d<float>                in2      [[texture(2)]],
    texture2d<float>                in3      [[texture(3)]],
    texture2d<float>                in4      [[texture(4)]],
    texture2d<float>                in5      [[texture(5)]],
    texture2d<float, access::write> out0     [[texture(6)]],
    texture2d<float, access::write> out1     [[texture(7)]],
    texture2d<float, access::write> out2     [[texture(8)]],
    texture2d<float, access::write> out3     [[texture(9)]],
    texture2d<float, access::write> out4     [[texture(10)]],
    texture2d<float, access::write> out5     [[texture(11)]],
    texture2d<float>                res_in   [[texture(12)]],
    device const float*             pw       [[buffer(0)]],
    constant NeuralConvCB&          cb       [[buffer(1)]],
    uint2                           gid      [[thread_position_in_grid]])
{
    const int2 dims = int2(cb.res.x, cb.res.y);
    if (gid.x >= uint(dims.x) || gid.y >= uint(dims.y)) return;

    const int in_c = int(cb.layer0.x);
    const int out_c = int(cb.layer0.y);
    const int w_off = int(cb.layer0.z);
    const int b_off = int(cb.layer0.w);
    const bool use_expm1 = cb.layer1.z > 0.5f;
    const bool residual = cb.layer1.w > 0.5f;

    float4 nb0[9], nb1[9], nb2[9], nb3[9], nb4[9], nb5[9];
    for (int tap = 0; tap < 9; ++tap) {
        int2 o = int2(tap % 3 - 1, tap / 3 - 1);
        int2 p = int2(gid) + o;
        bool inb = p.x >= 0 && p.y >= 0 && p.x < dims.x && p.y < dims.y;
        float4 z = float4(0.0f);
        nb0[tap] = (inb && in_c > 0)  ? in0.read(uint2(p)) : z;
        nb1[tap] = (inb && in_c > 4)  ? in1.read(uint2(p)) : z;
        nb2[tap] = (inb && in_c > 8)  ? in2.read(uint2(p)) : z;
        nb3[tap] = (inb && in_c > 12) ? in3.read(uint2(p)) : z;
        nb4[tap] = (inb && in_c > 16) ? in4.read(uint2(p)) : z;
        nb5[tap] = (inb && in_c > 20) ? in5.read(uint2(p)) : z;
    }

    float acc[24];
    for (int o = 0; o < out_c; ++o) acc[o] = pw[b_off + o];
    for (int o = 0; o < out_c; ++o) {
        int wb = w_off + o * in_c * 9;
        for (int q = 0; q < 4; ++q) {
            int c = q;
            if (c < in_c) {
                acc[o] += nb0[0][q] * pw[wb + c*9 + 0] + nb0[1][q] * pw[wb + c*9 + 1]
                        + nb0[2][q] * pw[wb + c*9 + 2] + nb0[3][q] * pw[wb + c*9 + 3]
                        + nb0[4][q] * pw[wb + c*9 + 4] + nb0[5][q] * pw[wb + c*9 + 5]
                        + nb0[6][q] * pw[wb + c*9 + 6] + nb0[7][q] * pw[wb + c*9 + 7]
                        + nb0[8][q] * pw[wb + c*9 + 8];
            }
            c = 4 + q;
            if (c < in_c) {
                acc[o] += nb1[0][q] * pw[wb + c*9 + 0] + nb1[1][q] * pw[wb + c*9 + 1]
                        + nb1[2][q] * pw[wb + c*9 + 2] + nb1[3][q] * pw[wb + c*9 + 3]
                        + nb1[4][q] * pw[wb + c*9 + 4] + nb1[5][q] * pw[wb + c*9 + 5]
                        + nb1[6][q] * pw[wb + c*9 + 6] + nb1[7][q] * pw[wb + c*9 + 7]
                        + nb1[8][q] * pw[wb + c*9 + 8];
            }
            c = 8 + q;
            if (c < in_c) {
                acc[o] += nb2[0][q] * pw[wb + c*9 + 0] + nb2[1][q] * pw[wb + c*9 + 1]
                        + nb2[2][q] * pw[wb + c*9 + 2] + nb2[3][q] * pw[wb + c*9 + 3]
                        + nb2[4][q] * pw[wb + c*9 + 4] + nb2[5][q] * pw[wb + c*9 + 5]
                        + nb2[6][q] * pw[wb + c*9 + 6] + nb2[7][q] * pw[wb + c*9 + 7]
                        + nb2[8][q] * pw[wb + c*9 + 8];
            }
            c = 12 + q;
            if (c < in_c) {
                acc[o] += nb3[0][q] * pw[wb + c*9 + 0] + nb3[1][q] * pw[wb + c*9 + 1]
                        + nb3[2][q] * pw[wb + c*9 + 2] + nb3[3][q] * pw[wb + c*9 + 3]
                        + nb3[4][q] * pw[wb + c*9 + 4] + nb3[5][q] * pw[wb + c*9 + 5]
                        + nb3[6][q] * pw[wb + c*9 + 6] + nb3[7][q] * pw[wb + c*9 + 7]
                        + nb3[8][q] * pw[wb + c*9 + 8];
            }
            c = 16 + q;
            if (c < in_c) {
                acc[o] += nb4[0][q] * pw[wb + c*9 + 0] + nb4[1][q] * pw[wb + c*9 + 1]
                        + nb4[2][q] * pw[wb + c*9 + 2] + nb4[3][q] * pw[wb + c*9 + 3]
                        + nb4[4][q] * pw[wb + c*9 + 4] + nb4[5][q] * pw[wb + c*9 + 5]
                        + nb4[6][q] * pw[wb + c*9 + 6] + nb4[7][q] * pw[wb + c*9 + 7]
                        + nb4[8][q] * pw[wb + c*9 + 8];
            }
            c = 20 + q;
            if (c < in_c) {
                acc[o] += nb5[0][q] * pw[wb + c*9 + 0] + nb5[1][q] * pw[wb + c*9 + 1]
                        + nb5[2][q] * pw[wb + c*9 + 2] + nb5[3][q] * pw[wb + c*9 + 3]
                        + nb5[4][q] * pw[wb + c*9 + 4] + nb5[5][q] * pw[wb + c*9 + 5]
                        + nb5[6][q] * pw[wb + c*9 + 6] + nb5[7][q] * pw[wb + c*9 + 7]
                        + nb5[8][q] * pw[wb + c*9 + 8];
            }
        }
    }

    float4 packs[6];
    for (int t = 0; t < 6; ++t) {
        float4 v = float4(0.0f);
        for (int q = 0; q < 4; ++q) {
            int o = t * 4 + q;
            if (o >= out_c) break;
            float val = acc[o];
            if (use_expm1) {
                val = (residual && o < 3)
                    ? exp(val + res_in.read(uint2(gid))[o]) - 1.0f
                    : exp(val) - 1.0f;
            } else {
                val = max(val, 0.0f);
            }
            v[q] = val;
        }
        packs[t] = v;
    }
    if (gid.y < 2 && gid.x < 2) {
        // 诊断探针（临时）：各输入纹理在 (0,0) 处的采样值写进 out0 前四像素。
        float s0 = in0.read(uint2(0)).x, s1 = in1.read(uint2(0)).x;
        float s2 = in2.read(uint2(0)).x, s3 = in3.read(uint2(0)).x;
        float s4 = in4.read(uint2(0)).x, s5 = in5.read(uint2(0)).x;
        float t0v = in0.read(uint2(0)).y, t1v = in1.read(uint2(0)).y;
        float4 v = (gid.y == 0) ? (gid.x == 0 ? float4(s0, s1, s2, s3)
                                              : float4(s4, s5, t0v, t1v))
                                : float4(cb.layer0.x, cb.layer0.y,
                                         cb.layer0.z, cb.layer0.w);
        out0.write(v, uint2(gid.x, gid.y));
    } else {
        out0.write(packs[0], gid);
    }
    out1.write(packs[1], gid);
    out2.write(packs[2], gid);
    out3.write(packs[3], gid);
    out4.write(packs[4], gid);
    out5.write(packs[5], gid);
}
