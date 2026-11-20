@group(0) @binding(0) var<uniform> p:Params;
@group(0) @binding(1) var albedo:texture_2d<f32>;
@group(0) @binding(2) var depthAtlas:texture_2d<f32>;
@group(0) @binding(3) var normalAtlas:texture_2d<f32>;
@group(0) @binding(4) var grid:texture_2d<f32>;
@group(0) @binding(5) var aux:texture_2d<f32>;
@group(0) @binding(6) var output:texture_storage_2d<rgba16float,write>;
@compute @workgroup_size(8,8) fn tile_reconstruct(@builtin(global_invocation_id) id:vec3<u32>){
 if(any(id.xy>=vec2<u32>(p.counts.x))){return;}let pixel=vec2<i32>(id.xy);let width=p.counts.x/p.counts.y;let own=textureLoad(aux,vec2<i32>(id.xy/p.counts.y),0);let card=u32(round(own.a));let n=octDecode(textureLoad(normalAtlas,pixel,0).rg);let d=textureLoad(depthAtlas,pixel,0).r;
 let g=(vec2<f32>(id.xy)+.5)/f32(p.counts.y)-.5;let base=vec2<i32>(floor(g));let f=fract(g);var sum=vec3<f32>(0);var weight=0.0;let band=max(p.extent.x/max(p.origin.w,1)*1.25,.30);
 for(var y=0;y<2;y++){for(var x=0;x<2;x++){let at=clamp(base+vec2<i32>(x,y),vec2<i32>(0),vec2<i32>(i32(width)-1));let w=select(1-f.x,f.x,x!=0)*select(1-f.y,f.y,y!=0);if(w<=0){continue;}let value=textureLoad(grid,at,0);if(value.a<=0){continue;}let evidence=textureLoad(aux,at,0);if(u32(round(evidence.a))!=card||dot(octDecode(evidence.rg),n)<.7||abs(evidence.b-d)>2*max(band,.30)){continue;}sum+=value.rgb*w;weight+=w;}}
 var result=vec4<f32>(0);if(weight>0){result=vec4<f32>(sum/weight*textureLoad(albedo,pixel,0).rgb,1);}
 textureStore(output,pixel,result);
}
