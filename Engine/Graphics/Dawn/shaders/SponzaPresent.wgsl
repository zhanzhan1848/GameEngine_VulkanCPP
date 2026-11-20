// Frozen FinalBlit: bilinear LDR copy only. Tone mapping already happened at
// internal resolution; no second exposure, transfer function or gamma here.
@group(0) @binding(0) var ldr:texture_2d<f32>;
@group(0) @binding(1) var linearClamp:sampler;
struct Vertex { @builtin(position) position:vec4<f32>, @location(0) uv:vec2<f32> }
@vertex fn present_vs(@builtin(vertex_index) i:u32)->Vertex{
    let p=array<vec2<f32>,3>(vec2<f32>(-1,-1),vec2<f32>(3,-1),vec2<f32>(-1,3));
    return Vertex(vec4<f32>(p[i],0,1),vec2<f32>(p[i].x*.5+.5,.5-p[i].y*.5));
}
@fragment fn present_fs(v:Vertex)->@location(0) vec4<f32>{return vec4<f32>(textureSampleLevel(ldr,linearClamp,v.uv,0).rgb,1);}
