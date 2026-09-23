/**
 * @file NeuralWeights.cpp
 * @brief .pnn 解析 + CPU 参考前向实现
 * @details 数值语义必须与 train_image_fit.py / MLPInferDebug.metal /
 *          MLPInfer.comp 保持一致（PRIMAL NEURAL SPEC v1）。本文件是规格的
 *          C++ 权威拷贝，单测（TestNeuralWeights）用 Python 导出的 parity
 *          向量锁住三端一致性。
 */
#include "NeuralWeights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <utility>

namespace primal::graphics::neural {

namespace {

// PRIMAL NEURAL SPEC v1 — hash 常量（勿改，四处同步）。
constexpr u32 kHashSalt = 0x9E3779B9u;
constexpr u32 kHashPrimeI = 0x85EBCA6Bu;
constexpr u32 kHashPrimeJ = 0xC2B2AE35u;
constexpr u32 kHashMix = 0x27D4EB2Fu;

constexpr u32 kEncodingFeatures = 2;

u32 Hash3(u32 i, u32 j, u32 l) {
    u32 h = l * kHashSalt;
    h ^= i * kHashPrimeI;
    h ^= j * kHashPrimeJ;
    h *= kHashMix;
    h ^= h >> 15;
    return h;
}

f32 ApplyActivation(Activation act, f32 x) {
    switch (act) {
    case Activation::Relu: return x > 0.0f ? x : 0.0f;
    case Activation::Sigmoid: return 1.0f / (1.0f + std::exp(-x));
    case Activation::None:
    default: return x;
    }
}

struct Reader {
    const u8* data;
    size_t size;
    size_t pos = 0;
    bool fail = false;

    template <typename T>
    T Read() {
        if (fail || pos + sizeof(T) > size) { fail = true; return T{}; }
        T v;
        std::memcpy(&v, data + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    void Skip(size_t bytes) {
        if (fail || pos + bytes > size) { fail = true; return; }
        pos += bytes;
    }
};

}  // namespace

bool ParsePnn(const u8* data, size_t size, NeuralNetwork& out, std::string* err) {
    auto set_err = [&](const char* msg) {
        if (err) *err = msg;
        return false;
    };
    if (!data || size < kPnnHeaderSize) return set_err("pnn: truncated header");

    Reader r{data, size};
    if (r.Read<u32>() != kPnnMagic) return set_err("pnn: bad magic");
    out = NeuralNetwork{};
    out.version = r.Read<u32>();
    if (out.version != 1 && out.version != 2) return set_err("pnn: unsupported version");
    const u32 layer_count = r.Read<u32>();
    const u32 table_count = r.Read<u32>();
    out.input_dim = r.Read<u32>();
    out.output_dim = r.Read<u32>();
    const u32 param_count = r.Read<u32>();
    out.flags = r.Read<u32>();   // v1 恒 0；v2 bit0 = residual_rgb
    if (r.fail) return set_err("pnn: truncated header");

    if (layer_count == 0 || layer_count > 16) return set_err("pnn: bad layer count");
    if (table_count > (64u << 20)) return set_err("pnn: table size implausible");

    out.level_res.resize(param_count);
    for (u32 i = 0; i < param_count; ++i) out.level_res[i] = r.Read<u32>();
    if (r.fail) return set_err("pnn: truncated encoding params");

    // v1 层描述 12B（dense 隐式）；v2 层描述 24B：kind,in,out(u32×3) +
    // act,kernel,stride,pad(u8×4) + flags,reserved(u32×2)。与
    // train_denoiser.py export_pnn_v2 的 "<IIIBBBBII" 逐字节镜像。
    const bool v2 = out.version >= 2;
    u32 max_dim = 0;
    out.layers.resize(layer_count);
    for (u32 l = 0; l < layer_count; ++l) {
        LayerDesc& d = out.layers[l];
        if (v2) {
            u32 kind = r.Read<u32>();
            d.in_dim = r.Read<u32>();
            d.out_dim = r.Read<u32>();
            const u8 act = r.Read<u8>();
            d.kernel = r.Read<u8>();
            d.stride = r.Read<u8>();
            d.pad = r.Read<u8>();
            d.flags = r.Read<u32>();
            r.Skip(4);   // reserved
            if (r.fail) return set_err("pnn: truncated layer desc");
            if (kind > 1) return set_err("pnn: unknown layer kind");
            d.kind = static_cast<LayerKind>(kind);
            d.activation = static_cast<Activation>(act);
        } else {
            d.in_dim = r.Read<u32>();
            d.out_dim = r.Read<u32>();
            const u8 act = r.Read<u8>();
            d.quant = r.Read<u8>();
            r.Skip(2);
            if (r.fail) return set_err("pnn: truncated layer desc");
            if (act > 2) return set_err("pnn: unknown activation");
            d.activation = static_cast<Activation>(act);
        }
        if (d.quant != 0) return set_err("pnn: only f32 quant supported");
        if (d.in_dim == 0 || d.out_dim == 0 || d.out_dim > 1024) {
            return set_err("pnn: implausible layer dims");
        }
        if (d.kind == LayerKind::Conv &&
            (d.kernel != 3 || d.stride != 1 || d.pad != 1)) {
            return set_err("pnn: only 3x3 s1 p1 conv supported");
        }
        max_dim = std::max(max_dim, d.out_dim);
    }
    // 层链闭合：首层吃 input_dim，末层吐 output_dim，逐层衔接。
    if (out.layers.front().in_dim != out.input_dim ||
        out.layers.back().out_dim != out.output_dim) {
        return set_err("pnn: layer chain does not match declared dims");
    }
    for (u32 l = 1; l < layer_count; ++l) {
        if (out.layers[l].in_dim != out.layers[l - 1].out_dim) {
            return set_err("pnn: layer chain broken");
        }
    }

    // 预算校验先于分配：table + 所有权重 f32 个数。
    size_t weight_floats = 0;
    for (const LayerDesc& d : out.layers) {
        const size_t k = (d.kind == LayerKind::Conv) ? (size_t)d.kernel * d.kernel : 1;
        weight_floats += (size_t)d.in_dim * d.out_dim * k + d.out_dim;
    }
    const size_t need_bytes = r.pos + (size_t)table_count * 4 + weight_floats * 4;
    if (need_bytes > size) return set_err("pnn: truncated payload");

    out.table.resize(table_count);
    if (table_count) {
        std::memcpy(out.table.data(), data + r.pos, (size_t)table_count * 4);
        r.Skip((size_t)table_count * 4);
    }
    out.weights.resize(weight_floats);
    if (weight_floats) {
        std::memcpy(out.weights.data(), data + r.pos, weight_floats * 4);
        r.Skip(weight_floats * 4);
    }
    u32 offset = 0;
    for (LayerDesc& d : out.layers) {
        d.w_offset = offset;
        const size_t k = (d.kind == LayerKind::Conv) ? (size_t)d.kernel * d.kernel : 1;
        offset += u32((size_t)d.in_dim * d.out_dim * k);
        d.b_offset = offset;
        offset += d.out_dim;
    }
    return true;
}

bool LoadPnnFile(const std::string& path, NeuralNetwork& out, std::string* err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        if (err) *err = "pnn: cannot open " + path;
        return false;
    }
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> bytes((size_t)size);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) {
        if (err) *err = "pnn: read failed " + path;
        return false;
    }
    return ParsePnn(bytes.data(), bytes.size(), out, err);
}

bool HashEncode(const NeuralNetwork& net, f32 u, f32 v, f32* feat) {
    const u32 levels = (u32)net.level_res.size();
    if (levels == 0 || net.table.empty()) return false;
    if (net.input_dim != levels * kEncodingFeatures + 2) return false;

    for (u32 l = 0; l < levels; ++l) {
        const f32 res = (f32)net.level_res[l];
        const f32 res_m1 = (f32)(net.level_res[l] - 1);
        const f32 qx = u * res;
        const f32 qy = v * res;
        const f32 flx = std::floor(qx);
        const f32 fly = std::floor(qy);
        const f32 i0 = flx > res_m1 ? res_m1 : flx;
        const f32 j0 = fly > res_m1 ? res_m1 : fly;
        const f32 fx = qx - flx;
        const f32 fy = qy - fly;
        const f32 i1 = i0 + 1.0f > res_m1 ? res_m1 : i0 + 1.0f;
        const f32 j1 = j0 + 1.0f > res_m1 ? res_m1 : j0 + 1.0f;

        // 每级表基址 = l * per_level，entries = per_level / F（须为 2 的幂）。
        const u32 per_level = (u32)(net.table.size() / levels);
        const u32 entries = per_level / kEncodingFeatures;
        const u32 mask = entries - 1;  // entries 必须是 2 的幂
        if (entries == 0 || (entries & mask) != 0) return false;

        const u32 h00 = Hash3((u32)i0, (u32)j0, l) & mask;
        const u32 h10 = Hash3((u32)i1, (u32)j0, l) & mask;
        const u32 h01 = Hash3((u32)i0, (u32)j1, l) & mask;
        const u32 h11 = Hash3((u32)i1, (u32)j1, l) & mask;
        const f32 w00 = (1.0f - fx) * (1.0f - fy);
        const f32 w10 = fx * (1.0f - fy);
        const f32 w01 = (1.0f - fx) * fy;
        const f32 w11 = fx * fy;
        const size_t base = (size_t)l * per_level;
        for (u32 c = 0; c < kEncodingFeatures; ++c) {
            const f32 f00 = net.table[base + (size_t)h00 * kEncodingFeatures + c];
            const f32 f10 = net.table[base + (size_t)h10 * kEncodingFeatures + c];
            const f32 f01 = net.table[base + (size_t)h01 * kEncodingFeatures + c];
            const f32 f11 = net.table[base + (size_t)h11 * kEncodingFeatures + c];
            feat[l * kEncodingFeatures + c] =
                w00 * f00 + w10 * f10 + w01 * f01 + w11 * f11;
        }
    }
    feat[levels * kEncodingFeatures + 0] = u;
    feat[levels * kEncodingFeatures + 1] = v;
    return true;
}

bool ReferenceForward(const NeuralNetwork& net, const f32* uv2, f32* out,
                      std::vector<f32>& scratch) {
    size_t max_dim = net.input_dim;
    for (const LayerDesc& d : net.layers) max_dim = std::max<size_t>(max_dim, d.out_dim);
    // 双缓冲各 max_dim，严格不重叠——ping-pong 若按 input_dim 偏移会让
    // 隐层写入区与读取区交叠（in-place aliasing 会静默污染后续神经元）。
    const size_t cap = max_dim;
    scratch.resize(cap * 2);

    f32* bufs[2] = {scratch.data(), scratch.data() + cap};
    if (!HashEncode(net, uv2[0], uv2[1], bufs[0])) return false;
    f32* src = bufs[0];
    f32* dst = bufs[1];

    for (const LayerDesc& d : net.layers) {
        const f32* w = net.weights.data() + d.w_offset;
        const f32* b = net.weights.data() + d.b_offset;
        for (u32 o = 0; o < d.out_dim; ++o) {
            const f32* row = w + (size_t)o * d.in_dim;
            f32 acc = b[o];
            for (u32 i = 0; i < d.in_dim; ++i) acc += src[i] * row[i];
            dst[o] = ApplyActivation(d.activation, acc);
        }
        f32* t = src;
        src = dst;
        dst = t;
    }
    for (u32 o = 0; o < net.output_dim; ++o) out[o] = src[o];
    return true;
}

bool ReferenceForwardConv(const NeuralNetwork& net, const f32* planes_in,
                          u32 width, u32 height, f32* planes_out,
                          std::vector<f32>& scratch) {
    if (net.layers.empty() || width == 0 || height == 0) return false;
    for (const LayerDesc& d : net.layers) {
        if (d.kind != LayerKind::Conv || d.kernel != 3 || d.stride != 1) return false;
    }
    size_t max_c = net.layers.front().in_dim;
    for (const LayerDesc& d : net.layers) max_c = std::max<size_t>(max_c, d.out_dim);

    // 双缓冲平面（各 max_c*w*h，严格不重叠——同 ReferenceForward 的教训）。
    const size_t plane = (size_t)width * height;
    scratch.resize(max_c * plane * 2);
    f32* bufs[2] = {scratch.data(), scratch.data() + max_c * plane};
    std::memcpy(bufs[0], planes_in, (size_t)net.layers.front().in_dim * plane * 4);
    f32* src = bufs[0];
    f32* dst = bufs[1];

    for (const LayerDesc& d : net.layers) {
        const f32* w = net.weights.data() + d.w_offset;
        const f32* b = net.weights.data() + d.b_offset;
        const bool expm1 = (d.flags & 1u) != 0;
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                for (u32 o = 0; o < d.out_dim; ++o) {
                    const f32* wo = w + (size_t)o * d.in_dim * 9;
                    f32 acc = b[o];
                    // 累加顺序 c → ky → kx（与 GPU kernel 同序）；边界 = zero-pad
                    // （与 torch Conv2d padding=1 一致，parity 位级对齐）。
                    for (u32 c = 0; c < d.in_dim; ++c) {
                        const f32* wc = wo + (size_t)c * 9;
                        for (int ky = -1; ky <= 1; ++ky) {
                            const int yy = int(y) + ky;
                            if (yy < 0 || yy >= int(height)) continue;
                            for (int kx = -1; kx <= 1; ++kx) {
                                const int xx = int(x) + kx;
                                if (xx < 0 || xx >= int(width)) continue;
                                acc += src[(size_t)c * plane + yy * width + xx] *
                                       wc[(ky + 1) * 3 + (kx + 1)];
                            }
                        }
                    }
                    f32 v = acc;
                    if (expm1) {
                        // net.flags bit0（residual_rgb）：训练组合是
                        // expm1(x_log + net_log) —— 输入 ch0..2 即 log1p(noisy)，
                        // 残差在 log 域相加后再 expm1 出线性 HDR。
                        if ((net.flags & 1u) && o < 3u)
                            v = std::expm1(v + planes_in[(size_t)o * plane + y * width + x]);
                        else
                            v = std::expm1(v);
                    } else if (d.activation == Activation::Relu) v = v > 0.0f ? v : 0.0f;
                    dst[(size_t)o * plane + y * width + x] = v;
                }
            }
        }
        f32* t = src;
        src = dst;
        dst = t;
    }
    std::memcpy(planes_out, src, (size_t)net.output_dim * plane * 4);
    return true;
}

}  // namespace primal::graphics::neural
