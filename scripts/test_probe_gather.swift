// GPU regression of the production gather: energy, sparse coverage, cell seams.
// Run from repository root: swift scripts/test_probe_gather.swift
import Foundation
import Metal
import simd
let d = MTLCreateSystemDefaultDevice()!
let q = d.makeCommandQueue()!
let shaderPath = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "Engine/Graphics/Metal/shaders/Lumen/ScreenProbeGather.metal"
let source = try String(contentsOfFile: shaderPath, encoding: .utf8)
let lib = try d.makeLibrary(source: source, options: nil)
let pipeline = try d.makeComputePipelineState(function: lib.makeFunction(name: "screen_probe_gather")!)
let w=256, h=128, gw=16, gh=8, count=128
func buffer(_ n:Int)->MTLBuffer { d.makeBuffer(length:n,options:.storageModeShared)! }
func texture(_ format:MTLPixelFormat)->MTLTexture {
 let desc=MTLTextureDescriptor.texture2DDescriptor(pixelFormat:format,width:w,height:h,mipmapped:false)
 desc.storageMode = .shared; desc.usage = [.shaderRead,.shaderWrite]
 return d.makeTexture(descriptor:desc)!
}
let depth=texture(.r32Float), normal=texture(.rgba32Float), output=texture(.rgba32Float)
let depths=[Float](repeating:0.5,count:w*h), normals=[SIMD4<Float>](repeating:SIMD4(0.5,0.5,1,1),count:w*h)
depths.withUnsafeBytes { depth.replace(region:MTLRegionMake2D(0,0,w,h),mipmapLevel:0,withBytes:$0.baseAddress!,bytesPerRow:w*4) }
normals.withUnsafeBytes { normal.replace(region:MTLRegionMake2D(0,0,w,h),mipmapLevel:0,withBytes:$0.baseAddress!,bytesPerRow:w*16) }
let positions=buffer(count*16), probeNormals=buffer(count*16), sh=buffer(count*64)
var global=[SIMD4<Float>](repeating:.zero,count:22)
for offset in [0,4] { for i in 0..<4 { global[offset+i][i]=1 } }
global[9]=SIMD4(Float(gw),Float(gh),16,48)
global[10]=SIMD4(50,2,Float(w),Float(h))
let ps=positions.contents().bindMemory(to:SIMD4<Float>.self,capacity:count)
let ns=probeNormals.contents().bindMemory(to:SIMD4<Float>.self,capacity:count)
let coeff=sh.contents().bindMemory(to:SIMD4<Float>.self,capacity:count*4)
func run(_ amplitude:Float,_ ramp:Bool,_ sparse:Bool, _ split:Bool = false)->[SIMD4<Float>] {
 var depthValues=[Float](repeating:0.5,count:w*h)
 if split { for y in 0..<h { for x in w/2..<w { depthValues[y*w+x]=0.8 } } }
 depthValues.withUnsafeBytes { depth.replace(region:MTLRegionMake2D(0,0,w,h),mipmapLevel:0,withBytes:$0.baseAddress!,bytesPerRow:w*4) }
 for p in 0..<count {
  let x=p%gw,y=p/gw
  ps[p]=SIMD4((Float(x)+0.5)/Float(gw)*2-1,1-(Float(y)+0.5)/Float(gh)*2,0.5, sparse && (x+y)%2 == 1 ? 0:1)
  if split && x >= gw/2 { ps[p].z=0.8 }
  ns[p]=SIMD4(0,0,1,1)
  let irradiance=amplitude*(split && x >= gw/2 ? 8 : (ramp ? 1+Float(x)*0.1:1))
  coeff[p*4]=SIMD4(repeating:irradiance/(0.282095*Float.pi));coeff[p*4].w=0.8
  for b in 1..<4 { coeff[p*4+b] = .zero }
 }
 let cb=q.makeCommandBuffer()!, e=cb.makeComputeCommandEncoder()!
 e.setComputePipelineState(pipeline)
 e.setTexture(depth,index:0);e.setTexture(normal,index:1);e.setTexture(output,index:2)
 e.setBuffer(positions,offset:0,index:0);e.setBuffer(sh,offset:0,index:2);e.setBuffer(probeNormals,offset:0,index:4)
 global.withUnsafeBytes { e.setBytes($0.baseAddress!,length:$0.count,index:3) }
 e.dispatchThreads(MTLSize(width:w,height:h,depth:1),threadsPerThreadgroup:MTLSize(width:8,height:8,depth:1));e.endEncoding()
 cb.commit();cb.waitUntilCompleted();precondition(cb.status == .completed)
 var pixels=[SIMD4<Float>](repeating:.zero,count:w*h)
 pixels.withUnsafeMutableBytes { output.getBytes($0.baseAddress!,bytesPerRow:w*16,from:MTLRegionMake2D(0,0,w,h),mipmapLevel:0) }
 return pixels
}
for sparse in [false,true] {
 for amplitude:Float in [0,1,8] {
  let pixels=run(amplitude,false,sparse)
  precondition(pixels.allSatisfy { abs($0.x-amplitude)<0.0001 && abs($0.w-0.8)<0.0001 },"Constant field/coverage failed")
 }
 let pixels=run(1,true,sparse)
 var maxStep:Float=0
 for y in 32..<96 { for x in 33..<223 { maxStep=max(maxStep,abs(pixels[y*w+x].x-pixels[y*w+x-1].x)) } }
 if maxStep >= 0.015 { print("FAIL Probe cell seam: \(maxStep)"); exit(1) }
 print("PASS sparse=\(sparse): black/unit/HDR constant field, confidence; ramp max pixel step=\(maxStep)")
}

// Parallel surfaces separated in depth must not exchange their lighting,
// even though they have identical normals and adjacent screen probes.
for sparse in [false,true] {
 let pixels=run(1,false,sparse,true)
 var error:Float=0
 for y in 32..<96 { for x in 112..<144 {
  error=max(error,abs(pixels[y*w+x].x-(x < 128 ? 1:8)))
 } }
 if error > 0.001 { print("FAIL cross-surface lighting error: \(error)"); exit(1) }
 print("PASS parallel depth discontinuity sparse=\(sparse), error=\(error)")
}
