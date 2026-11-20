// Native HZB reduction contract: standard [0,1] depth, frozen Vulkan min pyramid.
import Foundation
import Metal
func check(_ v:Bool,_ s:String){if !v{fputs("FAIL: \(s)\n",stderr);exit(1)}}
guard let dev=MTLCreateSystemDefaultDevice(),let q=dev.makeCommandQueue() else{fatalError("Metal unavailable")}
let path=CommandLine.arguments.count>1 ? CommandLine.arguments[1] : "Engine/Graphics/Metal/shaders/HZBGeneration.metal"
let options=MTLCompileOptions();options.fastMathEnabled=false
let lib=try dev.makeLibrary(source:try String(contentsOfFile:path,encoding:.utf8),options:options)
let pipe=try dev.makeComputePipelineState(function:lib.makeFunction(name:"generate_hzb_mip_level_basic")!)
func texture(_ w:Int,_ h:Int,_ usage:MTLTextureUsage)->MTLTexture{let d=MTLTextureDescriptor.texture2DDescriptor(pixelFormat:.r32Float,width:w,height:h,mipmapped:false);d.storageMode = .shared;d.usage=usage;return dev.makeTexture(descriptor:d)!}
var checks=0
for (w,h) in [(8,8),(5,3),(1,9),(9,1),(1,1),(32,16)] {
    var values=(0..<w*h).map{Float(($0*37+11)%101)/100};values[0]=0;if values.count>1{values[values.count-1]=1}
    let width=max(1,w/2),height=max(1,h/2),src=texture(w,h,.shaderRead),dst=texture(width,height,.shaderWrite)
    values.withUnsafeBytes{src.replace(region:MTLRegionMake2D(0,0,w,h),mipmapLevel:0,withBytes:$0.baseAddress!,bytesPerRow:w*4)}
    let c=q.makeCommandBuffer()!,e=c.makeComputeCommandEncoder()!;e.setComputePipelineState(pipe);e.setTexture(src,index:0);e.setTexture(dst,index:1);e.dispatchThreadgroups(MTLSize(width:(width+15)/16,height:(height+15)/16,depth:1),threadsPerThreadgroup:MTLSize(width:16,height:16,depth:1));e.endEncoding();c.commit();c.waitUntilCompleted();check(c.status == .completed,"GPU \(String(describing:c.error))")
    var actual=[Float](repeating:0,count:width*height);actual.withUnsafeMutableBytes{dst.getBytes($0.baseAddress!,bytesPerRow:width*4,from:MTLRegionMake2D(0,0,width,height),mipmapLevel:0)}
    for y in 0..<height{for x in 0..<width{var expected:Float=1;for dy in 0..<2{for dx in 0..<2{let sx=x*2+dx,sy=y*2+dy;if sx<w && sy<h{expected=min(expected,values[sy*w+sx])}}};check(actual[y*width+x].bitPattern==expected.bitPattern,"\(w)x\(h) pixel \(x),\(y) expected min \(expected), got \(actual[y*width+x])");checks+=1}}
}
print("PASS Metal HZB min: 6 dimensions, \(checks) exact texels")
