// Execute the production resolve and LightEval radiosity-read block on Metal.
// Verify full-atlas coverage, texel/material identity and valid black at tile
// sizes 8 and 16. Run with GPU access from the repository root.
import Foundation
import Metal
let base = "Engine/Graphics/Metal/shaders/Lumen/"
func read(_ file:String) throws -> String { try String(contentsOfFile:file,encoding:.utf8) }
let allocation = try read("Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp")
precondition(allocation.contains("outDesc.size = { atlas_size_, atlas_size_, 1 };"), "Resolve output must allocate the full atlas")
var source = try read(base+"SurfaceCacheIndirectResolve.metal")
source = source.replacingOccurrences(of:"#include \"../CommonTypes.metal\"",with:"struct GlobalShaderData { float4 unused; };")
source = source.replacingOccurrences(of:"#include \"SurfaceCacheData.metal\"",with:try read(base+"SurfaceCacheData.metal"))
let light = try read(base+"SurfaceCacheLightEval.metal")
let begin = light.range(of:"    float3 tileRadiance =")!.lowerBound
let end = light.range(of:"    // C3.5 slice 5+6:")!.lowerBound
let block = String(light[begin..<end])
source += """
struct ReadParams { SurfaceCacheParams sc_params; uint4 radiosity_gate; };
kernel void consume(texture2d<float,access::read> indirect_out [[texture(0)]],
                    device float4* values [[buffer(0)]], constant uint& tile [[buffer(1)]],
                    uint2 atlas_uv [[thread_position_in_grid]]) {
    ReadParams params; params.sc_params.atlas_size=64; params.radiosity_gate=uint4(1,0,tile,0);
    \(block)
    values[atlas_uv.y*64+atlas_uv.x]=float4(tileRadiance,1);
}
"""
guard let device=MTLCreateSystemDefaultDevice(),let queue=device.makeCommandQueue() else { exit(2) }
let library=try device.makeLibrary(source:source,options:nil)
let resolve=try device.makeComputePipelineState(function:library.makeFunction(name:"surfaceCacheIndirectResolve")!)
let consume=try device.makeComputePipelineState(function:library.makeFunction(name:"consume")!)
func texture()->MTLTexture {
 let desc=MTLTextureDescriptor.texture2DDescriptor(pixelFormat:.rgba32Float,width:64,height:64,mipmapped:false)
 desc.storageMode = .shared; desc.usage=[.shaderRead,.shaderWrite]
 return device.makeTexture(descriptor:desc)!
}
let albedo=texture(),output=texture(),previous=texture()
var colors=[SIMD4<Float>]()
for y in 0..<64 { for x in 0..<64 {
 colors.append(x<8 ? .zero : SIMD4(Float(x)/64,Float(y)/64,0.5,1))
} }
colors.withUnsafeBytes { albedo.replace(region:MTLRegionMake2D(0,0,64,64),mipmapLevel:0,withBytes:$0.baseAddress!,bytesPerRow:64*16) }
let dummy=device.makeBuffer(length:4096,options:.storageModeShared)!
let values=device.makeBuffer(length:64*64*16,options:.storageModeShared)!
for tile:UInt32 in [8,16] {
 let count=64/Int(tile)*64/Int(tile)
 let hits=device.makeBuffer(length:count*32,options:.storageModeShared)!
 let h=hits.contents().bindMemory(to:UInt32.self,capacity:count*8)
 for i in 0..<count { h[i*8+5]=2 }
 var p=[UInt32](repeating:0,count:20);p[0]=64;p[12]=tile;p[13]=1;p[15]=1
 let params=p.withUnsafeBytes { device.makeBuffer(bytes:$0.baseAddress!,length:$0.count)! }
 let command=queue.makeCommandBuffer()!,encoder=command.makeComputeCommandEncoder()!
 encoder.setComputePipelineState(resolve)
 for index in [0,2,3] { encoder.setBuffer(dummy,offset:0,index:index) }
 encoder.setBuffer(params,offset:0,index:1);encoder.setBuffer(hits,offset:0,index:4)
 encoder.setTexture(previous,index:0);encoder.setTexture(albedo,index:1);encoder.setTexture(previous,index:2)
 encoder.setTexture(output,index:3);encoder.setTexture(previous,index:6)
 encoder.dispatchThreads(MTLSize(width:64/Int(tile),height:64/Int(tile),depth:1),threadsPerThreadgroup:MTLSize(width:1,height:1,depth:1))
 encoder.endEncoding()
 let reader=command.makeComputeCommandEncoder()!
 reader.setComputePipelineState(consume);reader.setTexture(output,index:0);reader.setBuffer(values,offset:0,index:0)
 var t=tile;reader.setBytes(&t,length:4,index:1)
 reader.dispatchThreads(MTLSize(width:64,height:64,depth:1),threadsPerThreadgroup:MTLSize(width:8,height:8,depth:1))
 reader.endEncoding();command.commit();command.waitUntilCompleted()
 precondition(command.status == .completed,"GPU execution failed")
 let actual=values.contents().bindMemory(to:SIMD4<Float>.self,capacity:4096)
 var error:Float=0
 for i in 0..<4096 {
  let expected=colors[i]*SIMD4<Float>(1.1,1.152,1.2,0)
  error=max(error,abs(actual[i].x-expected.x),abs(actual[i].y-expected.y),abs(actual[i].z-expected.z))
 }
 print("Radiosity full atlas, tile=\(tile), max error=\(error)")
 precondition(error<0.00001,"Atlas domain/material identity mismatch")
}
print("PASS production resolve → LightEval read, full coverage and valid black")
