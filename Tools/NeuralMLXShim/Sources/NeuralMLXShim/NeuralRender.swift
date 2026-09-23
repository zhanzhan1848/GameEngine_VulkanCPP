// NeuralRender.swift — R1.2 NR transformer C ABI（MLX-DLSS MLXNeuralRenderer 封装
// + 引擎 7ch GI → vendor 16ch 特征适配）。
//
// 16ch 契约（Docs/2026-09-22-neural-rendering-inference-training-plan.md 附录 A，
// 源 = MLX-DLSS python/mlxdlss/features.py；本文件为等价 Swift 移植）：
//   0-2 确定性高斯噪声（网络像素坐标 + frame_index hash）
//   3   常数 1
//   4-6 缩放颜色 ((half(c)-0.5)*0.125)，c∈[0,1]，7-9 同副本
//   10-15 控制位 standard 档（0,1,1,-1,-1,0）
// 引擎输入 7ch（与 NeuralDenoisePack / train_denoiser 同布局）：
//   [0-2]=log1p(gi) rgb, [3-5]=normal xyz, [6]=depth/16（本 ABI 只消费 0-2，
//   normal/depth 留给未来投影式 adapter）。
// 输出：LDR [0,1] rgb（NR 残差语义 clip(color01 + 0.25*head)），CHW 平面。
// 网络 extent：vendor 对齐（≥320 且 %64，镜像延伸），shim 内部 pad/crop。
//
// ABI：
//   void*   neural_nr_create_ex(const char* package_dir, int32_t flags,
//                               char* err, int32_t err_cap);
//           flags bit0 = metalFused（f16 + 块编译，~5× 快于 eager；RepeatCheck
//           实测 720p p50 112.8ms vs f16-eager 514ms）
//   int32_t neural_nr_infer(void* handle, const float* planes7_chw,
//                           int32_t width, int32_t height, int32_t frame_index,
//                           float* out_rgb_chw, char* err, int32_t err_cap);
//   void    neural_nr_destroy(void* handle);

import Foundation
import MLX
import DLSSMLX
import DLSSCore

final class NRModel {
    let renderer: MLXNeuralRenderer
    init(packageDir: String, fused: Bool) throws {
        self.renderer = try MLXNeuralRenderer(
            packageURL: URL(fileURLWithPath: packageDir),
            executionMode: fused ? .metalFused : .eager,
            computePrecision: fused ? .float16 : .float32)
    }
}

// ---- features.py 等价移植（nr_adapter.py 的 Swift 版）----

private func dynamicShiftMix(_ v: UInt32) -> UInt32 {
    let shift = (v >> 28) &+ 4
    let mixed = v ^ (v >> shift)
    return mixed &* 0x108EF2D9
}

private func uniform24(_ v: UInt32) -> Float {
    let mixed = dynamicShiftMix(v)
    let bits = (mixed >> 30) ^ (mixed >> 8)
    return (Float(bits &+ 1)) * 5.960464477539063e-8
}

/// 确定性高斯噪声三通道（NHWC 平铺 h*w*3）。
func deterministicNoise(height: Int, width: Int, frameIndex: UInt32) -> [Float] {
    var out = [Float](repeating: 0, count: height * width * 3)
    for y in 0..<height {
        for x in 0..<width {
            var seed: UInt32 = UInt32(truncatingIfNeeded: y) &* 0xD8163841
            seed ^= UInt32(truncatingIfNeeded: x) &* 0x8DA6B343
            seed ^= frameIndex &* 0x9E3779B9
            seed ^= 0x243F6A88
            let multiplied = dynamicShiftMix(seed)
            let mixed = multiplied ^ (multiplied >> 22)
            let radiusAUniform = uniform24(mixed &* 0xCAA5B80D &+ 0x21DD796B)
            let angleBUniform = uniform24(mixed &* 0x83232C31 &+ 0x3463E0AC)
            let radiusBUniform = uniform24(mixed &* 0x2C9277B5 &+ 0xAC564B05)
            let angleAUniform = uniform24(mixed &* 0xFA6DC5F9 &+ 0x4712A88E)
            let radiusA = sqrt(max(0, -2 * log(max(radiusAUniform, 1e-9))))
            let radiusB = sqrt(max(0, -2 * log(max(radiusBUniform, 1e-9))))
            let tau: Float = 6.2831854820251465
            let angleA = tau * angleAUniform
            let angleB = tau * angleBUniform
            let i = (y * width + x) * 3
            out[i + 0] = radiusB * cos(angleA)
            out[i + 1] = radiusB * sin(angleA)
            out[i + 2] = radiusA * cos(angleB)
        }
    }
    return out
}

private func alignedExtent(_ extent: Int) -> Int {
    let minimum = max(320, extent)
    return (minimum + 63) / 64 * 64
}

/// 镜像延伸索引（features.py extendedIndices：2*extent-2-x，钳 0）。
private func extendedIndices(count: Int, extent: Int) -> [Int] {
    (0..<count).map { $0 < extent ? $0 : max(0, 2 * extent - 2 - $0) }
}

@_cdecl("neural_nr_create_ex")
public func neural_nr_create_ex(_ packageDir: UnsafePointer<CChar>?,
                                _ flags: Int32,
                                _ err: UnsafeMutablePointer<CChar>?,
                                _ errCap: Int32) -> UnsafeMutableRawPointer? {
    guard let packageDir else {
        writeErr(err, errCap, "null package dir")
        return nil
    }
    do {
        let model = try NRModel(packageDir: String(cString: packageDir),
                                fused: flags & 1 != 0)
        return Unmanaged.passRetained(model).toOpaque()
    } catch let e as ShimError {
        writeErr(err, errCap, e.description)
        return nil
    } catch {
        writeErr(err, errCap, "\(error)")
        return nil
    }
}

@_cdecl("neural_nr_create")
public func neural_nr_create(_ packageDir: UnsafePointer<CChar>?,
                             _ err: UnsafeMutablePointer<CChar>?,
                             _ errCap: Int32) -> UnsafeMutableRawPointer? {
    neural_nr_create_ex(packageDir, 0, err, errCap)
}

@_cdecl("neural_nr_destroy")
public func neural_nr_destroy(_ handle: UnsafeMutableRawPointer?) {
    guard let handle else { return }
    Unmanaged<NRModel>.fromOpaque(handle).release()
}

@_cdecl("neural_nr_infer")
public func neural_nr_infer(_ handle: UnsafeMutableRawPointer?,
                            _ planes: UnsafePointer<Float>?,
                            _ width: Int32, _ height: Int32, _ frameIndex: Int32,
                            _ outRgb: UnsafeMutablePointer<Float>?,
                            _ err: UnsafeMutablePointer<CChar>?,
                            _ errCap: Int32) -> Int32 {
    guard let handle, let planes, let outRgb else {
        writeErr(err, errCap, "null argument")
        return 1
    }
    let model = Unmanaged<NRModel>.fromOpaque(handle).takeUnretainedValue()
    let w = Int(width), h = Int(height)
    let plane = w * h

    // ---- 1. log1p 域 → 线性 HDR → tonemap 到 [0,1]（与 nr_adapter.py 同曲线）----
    var color01 = [Float](repeating: 0, count: plane * 3)
    for i in 0..<(plane * 3) {
        let linear = expm1(planes[i])
        color01[i] = min(max(1 - exp(-1.6 * linear), 0), 1)
    }

    // ---- 2. vendor 对齐 extent + 镜像延伸 ----
    let nw = alignedExtent(w), nh = alignedExtent(h)
    let rows = extendedIndices(count: nh, extent: h)
    let cols = extendedIndices(count: nw, extent: w)
    var extColor = [Float](repeating: 0, count: nh * nw * 3)
    for y in 0..<nh {
        for x in 0..<nw {
            let src = (rows[y] * w + cols[x]) * 3
            let dst = (y * nw + x) * 3
            extColor[dst + 0] = color01[src + 0]
            extColor[dst + 1] = color01[src + 1]
            extColor[dst + 2] = color01[src + 2]
        }
    }

    // ---- 3. 16ch 特征 NHWC ----
    var features = [Float](repeating: 0, count: nh * nw * 16)
    let noise = deterministicNoise(height: nh, width: nw, frameIndex: UInt32(bitPattern: frameIndex))
    for p in 0..<(nh * nw) {
        let f = p * 16, n = p * 3, c = p * 3
        features[f + 0] = noise[n + 0]
        features[f + 1] = noise[n + 1]
        features[f + 2] = noise[n + 2]
        features[f + 3] = 1
        let sx = (extColor[c] - 0.5) * 0.125
        let sy = (extColor[c + 1] - 0.5) * 0.125
        let sz = (extColor[c + 2] - 0.5) * 0.125
        features[f + 4] = sx; features[f + 5] = sy; features[f + 6] = sz
        features[f + 7] = sx; features[f + 8] = sy; features[f + 9] = sz
        features[f + 10] = 0; features[f + 11] = 1; features[f + 12] = 1
        features[f + 13] = -1; features[f + 14] = -1; features[f + 15] = 0
    }

    // ---- 4. 前向（actor async → 信号量桥接）----
    let input = MLXArray(features, [1, nh, nw, 16])
    let request: NeuralRenderRequest
    do {
        request = try NeuralRenderRequest(sequenceID: UInt64(bitPattern: Int64(frameIndex)),
                                          inputs: [try hostTensor(from: input)])
    } catch {
        writeErr(err, errCap, "request build failed: \(error)")
        return 2
    }
    let sem = DispatchSemaphore(value: 0)
    var headResult: NeuralRenderResult?
    var headError: Error?
    Task {
        do { headResult = try await model.renderer.render(request) }
        catch { headError = error }
        sem.signal()
    }
    sem.wait()
    if let headError {
        writeErr(err, errCap, "nr render failed: \(headError)")
        return 3
    }
    guard let result = headResult else {
        writeErr(err, errCap, "nr render returned nil")
        return 3
    }
    let head = result.outputs[0].bytes.withUnsafeBytes { raw -> [Float] in
        Array(raw.bindMemory(to: Float.self))
    }
    guard head.count >= nh * nw * 4 else {
        writeErr(err, errCap, "head size \(head.count) < \(nh * nw * 4)")
        return 4
    }

    // ---- 5. 裁回 + 残差 compose：out01 = clamp(color01 + 0.25*head_rgb) ----
    for y in 0..<h {
        for x in 0..<w {
            let src = (y * nw + x) * 4
            let c = (y * w + x) * 3
            for ch in 0..<3 {
                let composed = color01[c + ch] + 0.25 * head[src + ch]
                outRgb[ch * plane + y * w + x] = min(max(composed, 0), 1)
            }
        }
    }
    return 0
}

/// MLXArray → float32 NHWC HostTensor（契约名 "color"）。
private func hostTensor(from array: MLXArray) throws -> HostTensor {
    let f32 = array.asType(.float32)
    eval(f32)
    let shape = f32.shape
    let data = contiguous(f32).asData(access: .copy).data
    let desc = try TensorDescriptor(name: "color", shape: shape,
                                    dataType: .float32, layout: .nhwc)
    return try HostTensor(descriptor: desc, bytes: data)
}
