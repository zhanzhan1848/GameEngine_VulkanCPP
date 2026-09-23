// FrameGen.swift — M3 FrameGenerator C ABI（MLX-DLSS 的 DLSSMLX.FrameGenerator 封装）。
//
// 权重：mlxdlss-weights extract-fg（MLX-DLSS 仓库工具）从 libnvidia-ngx-dlss.so.310.7.0
// 提取的 framegen.safetensors（92 张量，dense [Cout,Cin,kh,kw] fp16）。
//
// ABI：
//   void*   neural_fg_create(const char* safetensors_path, char* err, int32_t err_cap);
//   void    neural_fg_destroy(void* handle);
//   int32_t neural_fg_interpolate(void* handle,
//                                 const float* frameA, const float* frameB,
//                                 int32_t width, int32_t height, float phase,
//                                 float* outFrame, char* err, int32_t err_cap);
// frames 为 NHWC [h][w][3] f32 [0,1]；outFrame 同布局（h×w×3）。

import Foundation
import MLX
import DLSSMLX

final class FGModel {
    let generator: FrameGenerator
    init(weights: [String: MLXArray]) throws {
        self.generator = try FrameGenerator(weights: weights, precision: .float16)
    }
}

/// 通用 safetensors → [String: MLXArray]（支持 F32/F16；FG 权重为 fp16）。
func loadSafetensorsDict(path: String) throws -> [String: MLXArray] {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    guard data.count > 8 else { throw ShimError.corrupt("file too small") }
    let headerLen = Int(data.withUnsafeBytes { $0.load(as: UInt64.self) })
    guard headerLen > 0, 8 + headerLen <= data.count else {
        throw ShimError.corrupt("bad header length \(headerLen)")
    }
    guard let header = try JSONSerialization.jsonObject(with: data.subdata(in: 8..<(8 + headerLen)))
        as? [String: Any] else {
        throw ShimError.corrupt("header JSON parse failed")
    }
    let dataStart = 8 + headerLen
    var out: [String: MLXArray] = [:]
    for (name, entry) in header where name != kMetadataKey {
        guard let t = entry as? [String: Any],
              let offs = t["data_offsets"] as? [Double], offs.count == 2,
              let dtype = t["dtype"] as? String,
              let shape = t["shape"] as? [Double] else {
            throw ShimError.corrupt("tensor \(name): bad header entry")
        }
        let begin = dataStart + Int(offs[0]), end = dataStart + Int(offs[1])
        guard begin >= dataStart, end <= data.count, begin <= end else {
            throw ShimError.corrupt("tensor \(name): offsets out of range")
        }
        let n = (end - begin) / 2
        let shapeInt = shape.map { Int($0) }
        switch dtype {
        case "F32":
            let n32 = (end - begin) / 4
            let v = [Float](unsafeUninitializedCapacity: n32) { buf, filled in
                data.withUnsafeBytes { raw in
                    for i in 0..<n32 { buf[i] = raw.load(fromByteOffset: begin + i * 4, as: Float.self) }
                }
                filled = n32
            }
            out[name] = MLXArray(v, shapeInt)
        case "F16":
            let n16 = (end - begin) / 2
            var f16 = [Float16](repeating: 0, count: n16)
            data.withUnsafeBytes { raw in
                for i in 0..<n16 { f16[i] = Float16(bitPattern: raw.load(fromByteOffset: begin + i * 2, as: UInt16.self)) }
            }
            out[name] = MLXArray(f16, shapeInt)
        default:
            throw ShimError.unsupported("tensor \(name): dtype \(dtype)")
        }
    }
    return out
}

@_cdecl("neural_fg_create")
public func neural_fg_create(_ path: UnsafePointer<CChar>?,
                             _ err: UnsafeMutablePointer<CChar>?,
                             _ errCap: Int32) -> UnsafeMutableRawPointer? {
    guard let path else {
        writeErr(err, errCap, "null path")
        return nil
    }
    do {
        let weights = try loadSafetensorsDict(path: String(cString: path))
        let model = try FGModel(weights: weights)
        return Unmanaged.passRetained(model).toOpaque()
    } catch let e as ShimError {
        writeErr(err, errCap, e.description)
        return nil
    } catch {
        writeErr(err, errCap, "\(error)")
        return nil
    }
}

@_cdecl("neural_fg_destroy")
public func neural_fg_destroy(_ handle: UnsafeMutableRawPointer?) {
    guard let handle else { return }
    Unmanaged<FGModel>.fromOpaque(handle).release()
}

@_cdecl("neural_fg_interpolate")
public func neural_fg_interpolate(_ handle: UnsafeMutableRawPointer?,
                                  _ frameA: UnsafePointer<Float>?,
                                  _ frameB: UnsafePointer<Float>?,
                                  _ width: Int32, _ height: Int32, _ phase: Float,
                                  _ outFrame: UnsafeMutablePointer<Float>?,
                                  _ err: UnsafeMutablePointer<CChar>?,
                                  _ errCap: Int32) -> Int32 {
    guard let handle, let frameA, let frameB, let outFrame else {
        writeErr(err, errCap, "null argument")
        return 1
    }
    let model = Unmanaged<FGModel>.fromOpaque(handle).takeUnretainedValue()
    let w = Int(width), h = Int(height)
    let count = w * h * 3
    let a = MLXArray([Float](UnsafeBufferPointer(start: frameA, count: count)), [1, h, w, 3])
    let b = MLXArray([Float](UnsafeBufferPointer(start: frameB, count: count)), [1, h, w, 3])
    do {
        let out = try model.generator.interpolate(a, b, phase: phase)   // [1,h,w,3] f32 [0,1]
        eval(out)
        let bytes = out.asData(access: .copy).data
        bytes.withUnsafeBytes { raw in
            memcpy(outFrame, raw.baseAddress, count * 4)
        }
        return 0
    } catch {
        writeErr(err, errCap, "\(error)")
        return 2
    }
}
