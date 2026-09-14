// GPU contract test: production Vulkan card transform/normal expressions,
// executed as MSL against all six card views and asymmetric atlas locations.
// This is a math contract test; the Vulkan Sponza capture tests rasterization.
import Foundation
import Metal
import simd

let directory = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "Engine/Graphics/Vulkan/shaders/Nanite"
let vertex = try String(contentsOfFile: directory + "/CardCapture.vert", encoding: .utf8)
let fragment = try String(contentsOfFile: directory + "/CardCapture.frag", encoding: .utf8)
let transform = String(vertex[vertex.range(of: "gl_Position = cb.proj")!.lowerBound...])
    .components(separatedBy: "}")[0].replacingOccurrences(of: "gl_Position", with: "position")
let normal = String(fragment[fragment.range(of: "vec3 N = normalize(IN.normal);")!.lowerBound..<fragment.range(of: "out_albedo =")!.lowerBound])
    .replacingOccurrences(of: "vec3", with: "float3")
    .replacingOccurrences(of: "IN.normal", with: "a.normal.xyz")
    .replacingOccurrences(of: "gl_FrontFacing", with: "(a.normal.w > 0.0)")
let source = """
#include <metal_stdlib>
using namespace metal;
struct Params { float4x4 view, proj; float4 center, fwd, misc; };
struct Input { Params cb; float4 world, normal; };
kernel void check(device const Input* data [[buffer(0)]],
                  device float4* output [[buffer(1)]], uint id [[thread_position_in_grid]]) {
    Input a = data[id]; Params cb = a.cb; float4 world_pos = a.world;
    float4 position;
    \(transform)
    \(normal)
    output[id] = float4(position.xy / position.w * 0.5 + 0.5, dot(N, cb.fwd.xyz), 0);
}
"""
struct Params { var view, proj: simd_float4x4; var center, fwd, misc: SIMD4<Float> }
struct Input { var cb: Params; var world, normal: SIMD4<Float> }
var inputs = [Input](), expected = [SIMD2<Float>]()
for axis in 0..<3 { for direction in [-1 as Float, 1] {
    var fwd = SIMD3<Float>(repeating: 0); fwd[axis] = direction
    let up: SIMD3<Float> = axis == 2 ? [0,-1,0] : [0,0,-1]
    let right = simd_cross(fwd, up), newUp = simd_cross(right, fwd)
    let view = simd_float4x4(columns: (
        SIMD4(right.x,newUp.x,-fwd.x,0), SIMD4(right.y,newUp.y,-fwd.y,0),
        SIMD4(right.z,newUp.z,-fwd.z,0), SIMD4(0,0,0,1)))
    let mirror = axis == 1 ? direction > 0 : direction < 0
    let proj = simd_float4x4(diagonal: SIMD4(mirror ? -0.5 : 0.5, 0.25, -0.1, 1))
    let cb = Params(view:view, proj:proj, center:.zero, fwd:SIMD4(fwd,0), misc:.zero)
    for uv: SIMD2<Float> in [SIMD2(0.17,0.23),SIMD2(0.81,0.72)] {
        var p = fwd * 0.5
        p[axis == 0 ? 1 : 0] = (uv.x - 0.5) * 4
        p[axis == 2 ? 1 : 2] = (uv.y - 0.5) * 8
        // Both authored sides and both raster windings must face the card eye.
        for sign: Float in [-1,1] { for winding: Float in [-1,1] {
            inputs.append(Input(cb:cb,world:SIMD4(p,1),normal:SIMD4(fwd*sign,winding)))
            expected.append(uv)
        } }
    }
} }
guard let device = MTLCreateSystemDefaultDevice() else {
    fputs("Metal device unavailable; run with GPU access.\n", stderr); exit(2)
}
let library = try device.makeLibrary(source:source,options:nil)
let pipeline = try device.makeComputePipelineState(function:library.makeFunction(name:"check")!)
let input = inputs.withUnsafeBytes { device.makeBuffer(bytes:$0.baseAddress!,length:$0.count)! }
let output = device.makeBuffer(length:inputs.count*16,options:.storageModeShared)!
let command = device.makeCommandQueue()!.makeCommandBuffer()!
let encoder = command.makeComputeCommandEncoder()!
encoder.setComputePipelineState(pipeline)
encoder.setBuffer(input,offset:0,index:0); encoder.setBuffer(output,offset:0,index:1)
encoder.dispatchThreads(MTLSize(width:inputs.count,height:1,depth:1),threadsPerThreadgroup:MTLSize(width:1,height:1,depth:1))
encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
precondition(command.status == .completed, "GPU execution failed")
let result = output.contents().bindMemory(to:SIMD4<Float>.self,capacity:inputs.count)
var uvError: Float = 0, normalError: Float = 0
for i in inputs.indices {
    uvError = max(uvError,simd_length(SIMD2(result[i].x,result[i].y)-expected[i]))
    normalError = max(normalError,abs(result[i].z+1))
}
print("Card capture: UV error=\(uvError), normal error=\(normalError), \(inputs.count) cases")
if uvError > 0.00001 || normalError > 0.00001 { exit(1) }
print("PASS six card orientations, asymmetric UVs, two-sided normals and mirrored windings")
