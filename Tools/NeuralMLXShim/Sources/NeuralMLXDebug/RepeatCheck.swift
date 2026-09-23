// RepeatCheck — NR/FG 推理探针（R1.1 原生前向验证 + R1.3/R2.2 性能基准）
//
// 用法：swift run RepeatCheck
// REPEATCHECK_MODES="f32-eager:1280x720,fg:1280x720" 时只跑指定组（独立进程
// 冷跑用——持续 GPU 负载下 M4 Pro 热节流可让数字差 3-6×，2026-09-22 实测；
// 跨模式对比必须各自独立进程）。"nrnative" 单独触发 R1.1 原生前向统计。
// 输出（stderr）：
//   [NR load]    模型加载 + temporalCadence
//   [NR native]  NVIDIA 原生 16ch 特征前向：形状/非零/NaN 统计（R1.1 验证）
//   [NR bench]   f32 / f16 / int8-fast 三模式 head-only 计时（R1.3，1280×720
//                与 2560×1440 两个分辨率，自适应迭代数）
//   [FG bench]   FrameGenerator 64×64 机制探针 + 1280×720 全分辨率计时（R2.2）
import Foundation
import MLX
import DLSSMLX
import DLSSCore

let repoRoot = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP"
let nrPackage = URL(fileURLWithPath: "\(repoRoot)/weights/nr/NeuralRendering.dlssmodel")
let fgWeights = "\(repoRoot)/weights/fg/framegen.safetensors"
let modeSelection = ProcessInfo.processInfo.environment["REPEATCHECK_MODES"] ?? ""

func modeSelected(_ name: String, _ width: Int, _ height: Int) -> Bool {
    if modeSelection.isEmpty { return true }
    return modeSelection.split(separator: ",").contains {
        let tag = "\($0)".lowercased()
        return tag == "\(name):\(width)x\(height)".lowercased() || tag == name.lowercased()
    }
}

func buildColorTensor(width: Int, height: Int) throws -> HostTensor {
    // 程序化测试图：平滑渐变 + 棋盘细节，值域 [0,1]（FirstFrame 契约输入）。
    var pixels = [Float](repeating: 0, count: height * width * 3)
    for y in 0..<height {
        for x in 0..<width {
            let i = (y * width + x) * 3
            pixels[i + 0] = Float(x % 256) / 255.0
            pixels[i + 1] = Float(y % 256) / 255.0
            let checker = Float((x / 16 + y / 16) % 2)
            pixels[i + 2] = 0.25 + 0.5 * checker
        }
    }
    let shape = [1, height, width, 3]
    let bytes = pixels.withUnsafeBufferPointer { Data(buffer: $0) }
    let desc = try TensorDescriptor(name: "color", shape: shape,
                                    dataType: .float32, layout: .nhwc)
    return try HostTensor(descriptor: desc, bytes: bytes)
}

func tensorStats(_ tensor: HostTensor, label: String) {
    let vals = tensor.bytes.withUnsafeBytes { raw -> [Float] in
        Array(raw.bindMemory(to: Float.self))
    }
    let nz = vals.filter { abs($0) > 1e-6 }.count
    let nan = vals.filter { $0.isNaN }.count
    let mean = vals.reduce(0, +) / Float(max(vals.count, 1))
    fputs("[\(label)] shape=\(tensor.descriptor.shape) nonzero=\(nz)/\(vals.count) "
        + "NaN=\(nan) mean=\(String(format: "%.4f", mean)) "
        + "min=\(String(format: "%.4f", vals.min() ?? -1)) "
        + "max=\(String(format: "%.4f", vals.max() ?? -1))\n", stderr)
}

// 自适应迭代数：先用 5 次探底，再在 ~25s 预算内取最多 100 次。
func timedIters(_ probeMs: Double) -> Int {
    guard probeMs > 0 else { return 10 }
    return max(5, min(100, Int(25000.0 / probeMs)))
}

func benchRenderer(name: String, mode: MLXExecutionMode, precision: MLXComputePrecision,
                   width: Int, height: Int) async {
    do {
        let renderer = try MLXNeuralRenderer(packageURL: nrPackage,
                                             executionMode: mode,
                                             computePrecision: precision)
        // ---- R1.1 原生前向验证（仅首组做）：vendor 特征构建 → head → 统计 ----
        if name.contains("f32") && width == 1280 {
            let color = try buildColorTensor(width: width, height: height)
            let geometry = try NeuralRenderingNetworkGeometry.vendorAligned(
                outputWidth: width, outputHeight: height)
            let features = try NeuralRenderingFirstFramePreprocessor.makeFeatureTensor(
                from: color, noiseFrameIndex: 0, geometry: geometry,
                normalizedStyle: 0, localToneStrength: 1, localStructureStrength: 1,
                automaticMask: nil, controlMask: nil)
            tensorStats(features, label: "NR features")
            let headResult = try await renderer.render(
                NeuralRenderRequest(sequenceID: 1, inputs: [features]))
            tensorStats(headResult.outputs[0], label: "NR head out")
        }
        // ---- R1.3 计时：head-only（特征张量复用，排除预处理）----
        let color = try buildColorTensor(width: width, height: height)
        let geometry = try NeuralRenderingNetworkGeometry.vendorAligned(
            outputWidth: width, outputHeight: height)
        let features = try NeuralRenderingFirstFramePreprocessor.makeFeatureTensor(
            from: color, noiseFrameIndex: 0, geometry: geometry,
            normalizedStyle: 0, localToneStrength: 1, localStructureStrength: 1,
            automaticMask: nil, controlMask: nil)
        let warm = ContinuousClock()
        func elapsedMs(since start: ContinuousClock.Instant) -> Double {
            let d = start.duration(to: warm.now)
            // attoseconds = 1e-18s；→ms 除以 1e15
            return Double(d.components.seconds) * 1000
                + Double(d.components.attoseconds) / 1e15
        }
        for _ in 0..<3 { _ = try await renderer.render(NeuralRenderRequest(sequenceID: 2, inputs: [features])) }
        var probe: Double = 0
        for _ in 0..<5 {
            let s = warm.now
            _ = try await renderer.render(NeuralRenderRequest(sequenceID: 3, inputs: [features]))
            probe = max(probe, elapsedMs(since: s))
        }
        let iters = timedIters(probe / 5)
        var samples: [Double] = []
        for _ in 0..<iters {
            let s = warm.now
            _ = try await renderer.render(NeuralRenderRequest(sequenceID: 4, inputs: [features]))
            samples.append(elapsedMs(since: s))
        }
        samples.sort()
        let mean = samples.reduce(0, +) / Double(samples.count)
        let p50 = samples[samples.count / 2]
        fputs("[NR bench] mode=\(name) \(width)x\(height) netExtent=\(geometry.networkWidth)x\(geometry.networkHeight) "
            + "iters=\(iters) mean=\(String(format: "%.1f", mean))ms "
            + "p50=\(String(format: "%.1f", p50))ms min=\(String(format: "%.1f", samples[0]))ms "
            + "fps=\(String(format: "%.1f", 1000.0 / p50))\n", stderr)
    } catch {
        fputs("[NR bench] mode=\(name) FAILED: \(error)\n", stderr)
    }
}

// ---- 1: NR 加载 + R1.1/R1.3 ----
fputs("[NR load] \(nrPackage.path)\n", stderr)
do {
    let renderer = try MLXNeuralRenderer(packageURL: nrPackage, executionMode: .eager,
                                         computePrecision: .float32)
    let cadence = await renderer.temporalCadence
    fputs("[NR load] OK cadence=\(cadence)\n", stderr)
} catch {
    fputs("[NR load] FAILED: \(error)\n", stderr)
}

if modeSelected("nrnative", 1280, 720) {
    await benchRenderer(name: "f32-eager", mode: .eager, precision: .float32, width: 1280, height: 720)
}
if modeSelected("f32-eager", 2560, 1440) {
    await benchRenderer(name: "f32-eager", mode: .eager, precision: .float32, width: 2560, height: 1440)
}
if modeSelected("f16-eager", 1280, 720) {
    await benchRenderer(name: "f16-eager", mode: .eager, precision: .float16, width: 1280, height: 720)
}
if modeSelected("f16-eager", 2560, 1440) {
    await benchRenderer(name: "f16-eager", mode: .eager, precision: .float16, width: 2560, height: 1440)
}
if modeSelected("int8-fast", 1280, 720) {
    await benchRenderer(name: "int8-fast", mode: .int8Fast, precision: .float32, width: 1280, height: 720)
}
if modeSelected("int8-fast", 2560, 1440) {
    await benchRenderer(name: "int8-fast", mode: .int8Fast, precision: .float32, width: 2560, height: 1440)
}
if modeSelected("metal-fused", 1280, 720) {
    await benchRenderer(name: "metal-fused", mode: .metalFused, precision: .float16, width: 1280, height: 720)
}
if modeSelected("f16-eager", 640, 384) {
    await benchRenderer(name: "f16-eager", mode: .eager, precision: .float16, width: 640, height: 384)
}
if modeSelected("f16-eager", 1280, 768) {
    await benchRenderer(name: "f16-eager", mode: .eager, precision: .float16, width: 1280, height: 768)
}

// ---- 2: FG interpolate（64×64 机制探针 + 1280×720 计时）----
if modeSelection.isEmpty || modeSelection.lowercased().contains("fg") {
do {
    let fgW = try loadProbe(fgWeights)
    let gen = try FrameGenerator(weights: fgW, precision: .float16)

    func gradientFrames(width: Int, height: Int, shift: Int) -> (MLXArray, MLXArray) {
        var a = [Float](repeating: 0, count: width * height * 3)
        var b = [Float](repeating: 0, count: width * height * 3)
        for y in 0..<height { for x in 0..<width {
            for c in 0..<3 {
                a[(y * width + x) * 3 + c] = Float(x) / Float(width - 1)
                b[(y * width + x) * 3 + c] = Float(min(x + shift, width - 1)) / Float(width - 1)
            }
        }}
        return (MLXArray(a, [1, height, width, 3]), MLXArray(b, [1, height, width, 3]))
    }

    // 机制探针（64×64）：synthesize + interpolate 非零/NaN 统计
    let (a64, b64) = gradientFrames(width: 64, height: 64, shift: 16)
    let coarse = try gen.synthesize(a64, b64, phase: 0.5)
    eval(coarse)
    let cVals = coarse.asData(access: .copy).data.withUnsafeBytes { raw -> [Float] in
        Array(raw.bindMemory(to: Float.self)) }
    fputs("[FG synthesize 64] count=\(cVals.count) nonzero=\(cVals.filter { $0 != 0 && !$0.isNaN }.count) "
        + "NaN=\(cVals.filter { $0.isNaN }.count)\n", stderr)
    let out64 = try gen.interpolate(a64, b64, phase: 0.5)
    eval(out64)
    let oVals = out64.asData(access: .copy).data.withUnsafeBytes { raw -> [Float] in
        Array(raw.bindMemory(to: Float.self)) }
    fputs("[FG interp 64] count=\(oVals.count) nonzero(>1e-4)=\(oVals.filter { $0 > 1e-4 }.count) "
        + "mean=\(String(format: "%.4f", oVals.reduce(0, +) / Float(max(oVals.count, 1)))) "
        + "max=\(String(format: "%.3f", oVals.max() ?? -1))\n", stderr)

    // R2.2：1280×720 全分辨率计时
    let (aHD, bHD) = gradientFrames(width: 1280, height: 720, shift: 48)
    _ = try gen.interpolate(aHD, bHD, phase: 0.5)   // warmup（含编译）
    eval(out64)
    let clock = ContinuousClock()
    var fgSamples: [Double] = []
    for _ in 0..<10 {
        let s = clock.now
        let o = try gen.interpolate(aHD, bHD, phase: 0.5)
        eval(o)
        let d = s.duration(to: clock.now)
        // attoseconds = 1e-18s；→ms 除以 1e15
        fgSamples.append(Double(d.components.seconds) * 1000
            + Double(d.components.attoseconds) / 1e15)
    }
    fgSamples.sort()
    let fgMean = fgSamples.reduce(0, +) / Double(fgSamples.count)
    fputs("[FG interp 1280x720] iters=10 mean=\(String(format: "%.1f", fgMean))ms "
        + "p50=\(String(format: "%.1f", fgSamples[fgSamples.count / 2]))ms "
        + "min=\(String(format: "%.1f", fgSamples[0]))ms "
        + "fps=\(String(format: "%.1f", 1000.0 / fgSamples[fgSamples.count / 2])) "
        + "budget16ms=\(fgSamples[fgSamples.count / 2] < 16 ? "PASS" : "FAIL")\n", stderr)
} catch {
    fputs("[FG] error: \(error)\n", stderr)
}
}

func loadProbe(_ path: String) throws -> [String: MLXArray] {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    let n = Int(data.withUnsafeBytes { $0.load(as: UInt64.self) })
    let header = try JSONSerialization.jsonObject(with: data.subdata(in: 8..<(8 + n))) as! [String: Any]
    let base = 8 + n
    var out: [String: MLXArray] = [:]
    for (name, entry) in header where name != "__metadata__" {
        let t = entry as! [String: Any]
        let offs = t["data_offsets"] as! [Double]
        let shape = (t["shape"] as! [Double]).map { Int($0) }
        let dtype = t["dtype"] as! String
        let b = base + Int(offs[0]), e = base + Int(offs[1])
        if dtype == "F16" {
            let cnt = (e - b) / 2
            var f16 = [Float16](repeating: 0, count: cnt)
            data.withUnsafeBytes { raw in
                for i in 0..<cnt { f16[i] = Float16(bitPattern: raw.load(fromByteOffset: b + i * 2, as: UInt16.self)) }
            }
            out[name] = MLXArray(f16, shape)
        } else {
            let cnt = (e - b) / 4
            var f32 = [Float](repeating: 0, count: cnt)
            data.withUnsafeBytes { raw in
                for i in 0..<cnt { f32[i] = raw.load(fromByteOffset: b + i * 4, as: Float.self) }
            }
            out[name] = MLXArray(f32, shape)
        }
    }
    return out
}

fputs("[done]\n", stderr)
