// Runs the production Metal SH projection and gather expression on the GPU.
// Usage: swift scripts/test_probe_energy.swift (from repository root).
import Foundation
import Metal
import simd

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
func source(_ name: String) throws -> String {
    try String(contentsOf: root.appendingPathComponent("Engine/Graphics/Metal/shaders/Lumen/\(name).metal"), encoding: .utf8)
}
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
    fatalError("Metal GPU unavailable")
}
let average = try source("ScreenProbeAverage")
let gather = try source("ScreenProbeGather")
let start = gather.range(of: "static inline float3 evalSHIrradiance")!.lowerBound
let end = gather.range(of: "static inline float3 evalSH(")!.lowerBound
let fixture = """
kernel void fixture(device float4* rays [[buffer(0)]],
                    device float4* normals [[buffer(3)]],
                    device float4* history [[buffer(4)]],
                    constant AvgConstants& c [[buffer(2)]],
                    constant float4& field [[buffer(5)]],
                    uint gid [[thread_position_in_grid]]) {
    if (gid >= c.totalProbes * c.raysPerProbe) return;
    uint p = gid / c.raysPerProbe, r = gid % c.raysPerProbe;
    float3 n = normals[p].xyz;
    float invNq = 1.0;
    float3 d = c.isMode > 0.5
        ? spgiImportanceDirection(r, c.raysPerProbe, p, c.frameSeed, n,
             history[p*4].rgb, history[p*4+1].rgb, history[p*4+2].rgb, history[p*4+3].rgb, invNq)
        : spgiJitterRotation(p, c.frameSeed) * fibonacciDirection(r, c.raysPerProbe);
    d = orientAroundNormal(d, n);
    float L = field.x * (1.0 + field.y * d.y);
    rays[gid] = float4(float3(L), max(dot(n,d),0.0) * invNq);
}
kernel void evaluate(device const float4* sh [[buffer(1)]],
                     device const float4* normals [[buffer(3)]],
                     device float4* result [[buffer(6)]],
                     uint p [[thread_position_in_grid]]) {
    result[p] = float4(evalSHIrradiance(sh[p*4].rgb,sh[p*4+1].rgb,
                        sh[p*4+2].rgb,sh[p*4+3].rgb,normals[p].xyz),sh[p*4].w);
}
"""
let library = try device.makeLibrary(source: average + String(gather[start..<end]) + fixture, options: nil)
func pipeline(_ name: String) throws -> MTLComputePipelineState {
    try device.makeComputePipelineState(function: library.makeFunction(name: name)!)
}
let prepare = try pipeline("fixture"), project = try pipeline("screen_probe_average"), evaluate = try pipeline("evaluate")
let count = 6144
func buffer(_ bytes: Int) -> MTLBuffer { device.makeBuffer(length: bytes, options: .storageModeShared)! }
let rays = buffer(count * 64 * 16), sh = buffer(count * 4 * 16)
let normals = buffer(count * 16), history = buffer(count * 4 * 16), result = buffer(count * 16)
memset(history.contents(), 0, history.length)
let ns = normals.contents().bindMemory(to: SIMD4<Float>.self, capacity: count)
for i in 0..<count {
    ns[i] = i % 3 == 0 ? SIMD4(1,0,0,1) : (i % 3 == 1 ? SIMD4(0,1,0,1) : SIMD4(0,0,1,1))
}
struct Constants { var count: UInt32; var rays: UInt32; var seed: Float; var mode: Float }
func run(_ mode: Float, _ amplitude: Float, _ slope: Float = 0, _ rayCount: Int = 64) -> [Double] {
    var c = Constants(count: UInt32(count), rays: UInt32(rayCount), seed: 42, mode: mode)
    var field = SIMD4<Float>(amplitude,slope,0,0)
    let command = queue.makeCommandBuffer()!
    for (state, threads, group) in [(prepare,count*rayCount,64),(project,count*64,64),(evaluate,count,64)] {
        let encoder = command.makeComputeCommandEncoder()!
        encoder.setComputePipelineState(state)
        for (index, b) in [(0,rays),(1,sh),(3,normals),(4,history),(6,result)] { encoder.setBuffer(b,offset:0,index:index) }
        encoder.setBytes(&c,length:MemoryLayout<Constants>.size,index:2)
        encoder.setBytes(&field,length:16,index:5)
        encoder.dispatchThreads(MTLSize(width:threads,height:1,depth:1),threadsPerThreadgroup:MTLSize(width:group,height:1,depth:1))
        encoder.endEncoding()
    }
    command.commit(); command.waitUntilCompleted()
    precondition(command.status == .completed, "GPU error: \(String(describing: command.error))")
    let output = result.contents().bindMemory(to: SIMD4<Float>.self, capacity: count)
    var means = [Double](repeating:0,count:3)
    for i in 0..<count { precondition(output[i].x.isFinite); means[i%3] += Double(output[i].x) / Double(count/3) }
    return means
}
for raysPerProbe in [32,48,64] {
    let uniform = run(0,1,0,raysPerProbe), importance = run(1,1,0,raysPerProbe)
    for v in uniform + importance { precondition(abs(v / Double.pi - 1) < 0.035, "Constant field loses energy: \(v)") }
    let hdr = run(1,8,0,raysPerProbe)
    let doubled = run(1,2,0,raysPerProbe), black = run(1,0,0,raysPerProbe)
    for i in 0..<3 { precondition(abs(hdr[i]-8*importance[i]) < 0.001); precondition(abs(doubled[i]-2*importance[i]) < 0.0001); precondition(abs(black[i]) < 0.00001) }
    let directionalUniform = run(0,1,0.5,raysPerProbe), directionalImportance = run(1,1,0.5,raysPerProbe)
    for i in 0..<3 { precondition(abs(directionalUniform[i]-directionalImportance[i]) < 0.06) }
    precondition(directionalImportance[1] > directionalImportance[0] + 0.5, "Directional SH lobe inverted")
    print("PASS rays=\(raysPerProbe) constant uniform=\(uniform) importance=\(importance) directional=\(directionalImportance); linearity/black/finite")
}
// History deliberately emphasizes +Z while the actual input stays constant.
// A proposal may reduce variance, but must never change the expected energy.
let hs = history.contents().bindMemory(to: SIMD4<Float>.self, capacity: count*4)
for i in 0..<count {
    hs[i*4] = SIMD4(354.49,354.49,354.49,1)
    hs[i*4+2] = SIMD4(202.6,202.6,202.6,1)
}
let guided = run(1,1,0,48)
for v in guided { precondition(abs(v / Double.pi - 1) < 0.035, "History proposal changes energy: \(v)") }
print("PASS history-guided constant field \(guided)")
