// Native card capture for the static Sponza host. Regular RenderMesh fetch uses
// low-half X normals; the frozen meshlet path repacks them high-half X.
struct CardParams { viewProjection:mat4x4<f32>,center:vec4<f32>,forward:vec4<f32>,tint:vec4<f32>,material:vec4<u32> }
@group(0) @binding(0) var<uniform> card:CardParams;
@group(0) @binding(1) var albedo:texture_2d<f32>;
@group(0) @binding(2) var albedoSampler:sampler;
struct Input {
 @location(0) position:vec3<f32>,@location(1) signs:u32,
 @location(2) normal:u32,@location(3) tangent:u32,@location(4) uv:vec2<f32>
}
struct Vertex { @builtin(position) position:vec4<f32>,@location(0) world:vec3<f32>,@location(1) normal:vec3<f32>,@location(2) uv:vec2<f32> }
fn finite(v:f32)->bool{return (bitcast<u32>(v)&0x7f800000u)!=0x7f800000u;}
fn finite3(v:vec3<f32>)->bool{return finite(v.x)&&finite(v.y)&&finite(v.z);}
fn unpackNormal(packed:u32,signs:u32)->vec3<f32>{
 let xy=vec2<f32>(f32(packed&65535u),f32(packed>>16u))*(2.0/65535.0)-1.0;
 let d=dot(xy,xy);if(d>1.0){return vec3<f32>(0,0,1);}
 return vec3<f32>(xy,sqrt(max(0.0,1.0-d))*(f32((signs>>24u)&2u)-1.0));
}
@vertex fn card_capture_vertex(v:Input)->Vertex{
 var out:Vertex;out.position=card.viewProjection*vec4<f32>(v.position,1.0);
 out.world=v.position;out.normal=normalize(unpackNormal(v.normal,v.signs));out.uv=vec2<f32>(v.uv.x,1.0-v.uv.y);return out;
}
fn octEncode(n:vec3<f32>)->vec2<f32>{
 var xy=n.xy/(abs(n.x)+abs(n.y)+abs(n.z));
 if(n.z<0.0){xy=(1.0-abs(xy.yx))*select(vec2<f32>(-1),vec2<f32>(1),xy>=vec2<f32>(0));}
 return xy*.5+.5;
}
struct Surface { @location(0) albedo:vec4<f32>,@location(1) normal:vec2<f32>,@location(2) depth:f32 }
@fragment fn card_capture_fragment(v:Vertex)->Surface{
 let color=textureSample(albedo,albedoSampler,fract(v.uv)).rgb*card.tint.rgb;
 let depth=dot(v.world-card.center.xyz,card.forward.xyz);
 if(!finite3(color)||!finite3(v.normal)||dot(v.normal,v.normal)<1e-8||!finite(depth)||depth<0.0){discard;}
 var n=normalize(v.normal);if(dot(n,card.forward.xyz)>0.0){n=-n;}
 let flags=card.material.x;let transmission=(((flags>>8u)&255u)+8u)/17u;let thickness=(((flags>>16u)&255u)+18u)/36u;
 var out:Surface;out.albedo=vec4<f32>(color,f32(128u|(transmission<<3u)|thickness)/255.0);out.normal=octEncode(n);out.depth=depth;return out;
}
