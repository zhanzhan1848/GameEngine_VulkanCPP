// Shim.swift — NeuralMLXShim：神经渲染推理的 MLX 后端（C ABI）。
//
// 对应 Docs/2026-09-15-neural-mlx-backend-plan.md M1-a：把 4 层 3×3 conv
// 残差降噪器（train_denoiser.py 产物，safetensors 部署）交给 MLX 执行，
// 替代裸 Metal compute kernel 路线的绑定/时序/描述符调试成本。
//
// ABI（与 Engine/Graphics/Neural/NeuralMLXBackend.h 的函数指针表逐项对应）：
//   int32_t neural_mlx_api_version(void);
//   void*   neural_mlx_create(const char* safetensors_path, char* err, int32_t err_cap);
//   void    neural_mlx_destroy(void* handle);
//   int32_t neural_mlx_infer_f32(void* h, const float* in, int32_t ic, int32_t w, int32_t hh,
//                                float* out, int32_t oc, int32_t flags, char* err, int32_t cap);
//   int32_t neural_mlx_capabilities(void);
//
// 数据布局：in/out 均为平面 CHW [c][y][x] f32 连续（与 CPU 参考前向
// ReferenceForwardConv、引擎 pack 输出一致的引擎内部约定）。
// flags bit0 = expm1 输出（parity 模式，对齐 denoiser_parity.bin 的 torch
// 期望）；缺省（0）输出 log 域（引擎消费端执行 expm1，fp16 安全）。

import Foundation
import MLX
import MLXNN

// ---------------------------------------------------------------------------
// 模型：从 safetensors 重建（权重 + 计算图元数据）
// ---------------------------------------------------------------------------

/// 一层 3×3 conv。MLX 权重布局 [kH, kW, inC, outC]（NHWC 惯例），
/// torch/pnn 的 [out, in, k, k] 需 transpose(2,3,1,0)。
/// int8 模式（createFlags bit0）：权重按出通道对称量化为 uint8 常驻，
/// 前向时 dequant（MLX 图编译会把 cast+乘法融合进 conv 邻域）。
struct ConvLayer {
    var weight: MLXArray          // f32 模式 [O,k,k,I]
    var weightQ: MLXArray? = nil  // int8 模式 uint8 [O,k,k,I]
    var scale: MLXArray? = nil    // int8 模式 f32 [O,1,1,1]
    var bias: MLXArray
    var relu: Bool
    var expm1: Bool

    /// 前向取有效权重（f32 直接 / int8 现场反量化）。
    var effectiveWeight: MLXArray {
        guard let q = weightQ, let sc = scale else { return weight }
        return q.asType(Float.self) * sc
    }
}

final class NeuralModel {
    var layers: [ConvLayer] = []
    var inputDim = 0
    var outputDim = 0
    /// pnn 头 flags bit0 = residual_rgb：末层结果加输入 ch0..2（log 域）再出。
    var residualRgb = false
    /// createFlags bit0：权重 int8 量化存储（逐出通道对称）。
    var int8Weights = false

    /// 纯网络层前向（不含残差/expm1）。输入 CHW [c,h,w]；输出 CHW [out_c,h,w]。
    func forward(_ x: MLXArray) -> MLXArray {
        // [c,h,w] → NHWC [1,h,w,c]
        var h = x.transposed(1, 2, 0).expandedDimensions(axis: 0)
        for layer in layers {
            // conv2d：MLX 模块函数，权重 [C_out,kH,kW,C_in]（NHWC）；
            // bias 按 [1,1,1,C_out] 广播相加。
            h = conv2d(h, layer.effectiveWeight, stride: 1, padding: 1)
                + layer.bias.reshaped([1, 1, 1, -1])
            if layer.relu { h = maximum(h, 0) }
            // 注意：layer.expm1 不在此处应用——pnn v2 契约里 expm1 只出现在
            // 末层旗标，语义 = "输出变换"，由消费端选择（ABI flags bit0）。
            // 在这里应用会导致双重 expm1（2026-09-22 对拍定位）。
        }
        // [1,h,w,out_c] → [out_c,h,w]
        h = h.squeezed(axis: 0).transposed(2, 0, 1)
        return h
    }

    /// 2×2 平均池化（半分辨率降采样）。[c,h,w] → [c,h/2,w/2]（奇数截断）。
    private func pool2x2(_ x: MLXArray) -> MLXArray {
        let c = x.shape[0]
        let h2 = x.shape[1] / 2, w2 = x.shape[2] / 2
        let nhwc = x.transposed(1, 2, 0)                       // [h,w,c]
        let even = nhwc[0..<h2 * 2, 0..<w2 * 2]
        let r = even.reshaped([h2, 2, w2, 2, c])
        return r.mean(axes: [1, 3]).transposed(2, 0, 1)        // [c,h2,w2]
    }

    /// 完整前向（残差 + 半分辨率）。输入 CHW [c,h,w]。
    /// 返回 CHW [out_c, h', w']：全分辨率时 h'=h；半分辨率时残差/网络均在
    /// 半分辨率上组合，nearest 上采样回 h/2*2（奇数边缘由调用方 CPU 钳制）。
    func forwardCombined(_ x: MLXArray, halfRes: Bool) -> MLXArray {
        let working = halfRes ? pool2x2(x) : x
        var result = forward(working)
        if residualRgb {
            result = concatenated([result[0..<3] + working[0..<3], result[3...]],
                                  axis: 0)
        }
        if halfRes {
            result = repeated(result, count: 2, axis: 1)
            result = repeated(result, count: 2, axis: 2)
        }
        return result
    }
}

// ---------------------------------------------------------------------------
// safetensors 解析（自包含：不依赖 MLX 的 loadWeights，元数据必须自取）
// ---------------------------------------------------------------------------

let kMetadataKey = "__metadata__"

func loadNeuralModel(path: String) throws -> NeuralModel {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    guard data.count > 8 else { throw ShimError.corrupt("file too small") }
    let headerLen = Int(data.withUnsafeBytes { $0.load(as: UInt64.self) })
    guard headerLen > 0, 8 + headerLen <= data.count else {
        throw ShimError.corrupt("bad header length \(headerLen)")
    }
    let headerData = data.subdata(in: 8..<(8 + headerLen))
    guard let header = try JSONSerialization.jsonObject(with: headerData)
        as? [String: Any] else {
        throw ShimError.corrupt("header JSON parse failed")
    }
    guard let meta = header[kMetadataKey] as? [String: String] else {
        throw ShimError.corrupt("missing __metadata__")
    }
    guard let inputDim = Int(meta["input_dim"] ?? ""),
          let outputDim = Int(meta["output_dim"] ?? ""),
          let layerCount = Int(meta["layer_count"] ?? ""),
          let netFlags = Int(meta["net_flags"] ?? "") else {
        throw ShimError.corrupt("metadata missing dims/flags")
    }

    let dataStart = 8 + headerLen
    func tensorF32(_ name: String) throws -> [Float] {
        guard let t = header[name] as? [String: Any],
              let offs = t["data_offsets"] as? [Double], offs.count == 2,
              t["dtype"] as? String == "F32" else {
            throw ShimError.corrupt("tensor \(name) missing or not F32")
        }
        let begin = dataStart + Int(offs[0]), end = dataStart + Int(offs[1])
        guard begin >= dataStart, end <= data.count, begin <= end else {
            throw ShimError.corrupt("tensor \(name) offsets out of range")
        }
        let n = (end - begin) / 4
        return [Float](unsafeUninitializedCapacity: n) { buf, filled in
            data.withUnsafeBytes { raw in
                for i in 0..<n {
                    buf[i] = raw.load(fromByteOffset: begin + i * 4, as: Float.self)
                }
            }
            filled = n
        }
    }

    let model = NeuralModel()
    model.inputDim = inputDim
    model.outputDim = outputDim
    model.residualRgb = (netFlags & 1) != 0

    for i in 0..<layerCount {
        guard let spec = meta["layer\(i)"] else {
            throw ShimError.corrupt("layer\(i) spec missing")
        }
        var kind = "dense", act = "none", flags = ""
        for field in spec.split(separator: ",") {
            let kv = field.split(separator: "=", maxSplits: 1)
            guard kv.count == 2 else { continue }
            switch String(kv[0]) {
            case "kind": kind = String(kv[1])
            case "act": act = String(kv[1])
            case "flags": flags = String(kv[1])
            default: break
            }
        }
        guard kind == "conv" else {
            throw ShimError.unsupported("layer\(i) kind=\(kind) (仅 conv 网络当前受支持)")
        }
        let w = try tensorF32("layer\(i).weight")   // [out,in,k,k]
        let b = try tensorF32("layer\(i).bias")
        guard let wt = header["layer\(i).weight"] as? [String: Any],
              let shape = wt["shape"] as? [Double], shape.count == 4 else {
            throw ShimError.corrupt("layer\(i).weight shape")
        }
        let oC = Int(shape[0]), iC = Int(shape[1]), k = Int(shape[2])
        // torch [out,in,k,k] → mlx [out,k,k,in]
        var reord = [Float](repeating: 0, count: w.count)
        for o in 0..<oC {
            for c in 0..<iC {
                for ky in 0..<k {
                    for kx in 0..<k {
                        reord[((o * k + ky) * k + kx) * iC + c] =
                            w[((o * iC + c) * k + ky) * k + kx]
                    }
                }
            }
        }
        let wFull = MLXArray(reord, [oC, k, k, iC])
        var layer = ConvLayer(
            weight: wFull,
            bias: MLXArray(b, [oC]),
            relu: act == "relu",
            expm1: flags.contains("expm1"))
        if model.int8Weights {
            // 逐出通道对称量化：scale_o = max|w_o|/127（w 平面布局 [O,k,k,I]，
            // 出通道 o 的全部元素连续？否——[O,k,k,I] 中 o 的步长 = k*k*iC，
            // MLX max/除法按 [O,1,1,1] 广播完成，无需手动重排）。
            let absMax = maximum(abs(wFull).max(axes: [1, 2, 3], keepDims: true),
                                 1e-12)
            let scale = absMax / 127.0
            let q = clip(wFull / scale, min: -127.0, max: 127.0).round()
            layer.weightQ = q.asType(Int8.self)
            layer.scale = scale
        }
        model.layers.append(layer)
    }
    return model
}

enum ShimError: Error, CustomStringConvertible {
    case corrupt(String)
    case unsupported(String)
    case runtime(String)

    var description: String {
        switch self {
        case .corrupt(let m): return "corrupt: \(m)"
        case .unsupported(let m): return "unsupported: \(m)"
        case .runtime(let m): return "runtime: \(m)"
        }
    }
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

func writeErr(_ err: UnsafeMutablePointer<CChar>?, _ cap: Int32, _ msg: String) {
    guard let err, cap > 0 else { return }
    let bytes = Array(msg.utf8.prefix(Int(cap) - 1))
    _ = bytes.withUnsafeBufferPointer { buf -> Int32 in
        memcpy(err, buf.baseAddress, buf.count)
        return 0
    }
    err[Int(cap) - 1] = 0
}

@_cdecl("neural_mlx_api_version")
public func neural_mlx_api_version() -> Int32 { 1 }

@_cdecl("neural_mlx_capabilities")
public func neural_mlx_capabilities() -> Int32 {
    // bit0 API v1；bit1 半分辨率执行（shim 内 downsize→infer→upsample）；
    // bit2 int8 量化权重加载。
    return 1 | 2 | 4
}

@_cdecl("neural_mlx_create")
public func neural_mlx_create(_ path: UnsafePointer<CChar>?,
                              _ err: UnsafeMutablePointer<CChar>?,
                              _ errCap: Int32) -> UnsafeMutableRawPointer? {
    guard let path else {
        writeErr(err, errCap, "null path")
        return nil
    }
    let p = String(cString: path)
    do {
        let model = try loadNeuralModel(path: p)
        // 预 eval 权重，首次推理不做编译开销
        for l in model.layers {
            eval(l.weight, l.bias)
        }
        let box = Unmanaged.passRetained(model).toOpaque()
        return box
    } catch let e as ShimError {
        writeErr(err, errCap, e.description)
        return nil
    } catch {
        writeErr(err, errCap, "\(error)")
        return nil
    }
}

@_cdecl("neural_mlx_destroy")
public func neural_mlx_destroy(_ handle: UnsafeMutableRawPointer?) {
    guard let handle else { return }
    Unmanaged<NeuralModel>.fromOpaque(handle).release()
}

@_cdecl("neural_mlx_create_ex")
public func neural_mlx_create_ex(_ path: UnsafePointer<CChar>?,
                                 _ createFlags: Int32,
                                 _ err: UnsafeMutablePointer<CChar>?,
                                 _ errCap: Int32) -> UnsafeMutableRawPointer? {
    guard let path else {
        writeErr(err, errCap, "null path")
        return nil
    }
    let p = String(cString: path)
    do {
        let model = try loadNeuralModel(path: p)
        model.int8Weights = (createFlags & 1) != 0
        if model.int8Weights {
            // 量化发生在 loadNeuralModel 之后：重建层权重（此处按层重做，
            // 便于与 load 逻辑解耦）。逐出通道对称量化。
            for i in 0..<model.layers.count {
                let wFull = model.layers[i].weight
                let absMax = maximum(abs(wFull).max(axes: [1, 2, 3], keepDims: true),
                                     1e-12)
                let scale = absMax / 127.0
                let q = clip(wFull / scale, min: -127.0, max: 127.0).round()
                model.layers[i].weightQ = q.asType(Int8.self)
                model.layers[i].scale = scale
            }
        }
        for l in model.layers {
            eval(l.effectiveWeight, l.bias)
        }
        return Unmanaged.passRetained(model).toOpaque()
    } catch let e as ShimError {
        writeErr(err, errCap, e.description)
        return nil
    } catch {
        writeErr(err, errCap, "\(error)")
        return nil
    }
}

@_cdecl("neural_mlx_infer_f32")
public func neural_mlx_infer_f32(_ handle: UnsafeMutableRawPointer?,
                                 _ planesIn: UnsafePointer<Float>?,
                                 _ inC: Int32, _ width: Int32, _ height: Int32,
                                 _ planesOut: UnsafeMutablePointer<Float>?,
                                 _ outC: Int32,
                                 _ flags: Int32,
                                 _ err: UnsafeMutablePointer<CChar>?,
                                 _ errCap: Int32) -> Int32 {
    guard let handle, let planesIn, let planesOut else {
        writeErr(err, errCap, "null argument")
        return 1
    }
    let model = Unmanaged<NeuralModel>.fromOpaque(handle).takeUnretainedValue()
    let w = Int(width), h = Int(height), ic = Int(inC), oc = Int(outC)
    let plane = w * h
    guard plane > 0, ic > 0, oc > 0, ic == model.inputDim, oc == model.outputDim else {
        writeErr(err, errCap, "dim mismatch: net \(model.inputDim)→\(model.outputDim), call \(ic)→\(oc)")
        return 2
    }

    // CHW 平面 → [ic, h, w]
    let inputFloats = [Float](UnsafeBufferPointer(start: planesIn, count: ic * plane))
    let x = MLXArray(inputFloats, [ic, h, w])
    // flags bit0 = expm1 输出（parity 模式）；bit1 = 半分辨率执行。
    let halfRes = (flags & 2) != 0
    var result = model.forwardCombined(x, halfRes: halfRes)
    if flags & 1 != 0 {
        result = expm1(result)
    }
    eval(result)

    // GPU 结果 → CHW 平面输出。半分辨率时 GPU 尺寸 = (h/2*2, w/2*2) ≤
    // (h, w)，奇数边缘最后一行/列钳制复制（asData .copy = GPU→CPU 回读）。
    let gotH = halfRes ? (h / 2) * 2 : h
    let gotW = halfRes ? (w / 2) * 2 : w
    let bytes = result.asData(access: .copy).data
    bytes.withUnsafeBytes { raw in
        let src = raw.bindMemory(to: Float.self)
        for c in 0..<oc {
            for y in 0..<h {
                let sy = min(y, gotH - 1)
                for x in 0..<w {
                    let sx = min(x, gotW - 1)
                    planesOut[c * plane + y * w + x] =
                        src[c * gotH * gotW + sy * gotW + sx]
                }
            }
        }
    }
    return 0
}
