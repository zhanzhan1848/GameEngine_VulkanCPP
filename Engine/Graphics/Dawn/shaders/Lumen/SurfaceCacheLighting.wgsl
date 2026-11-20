// Native WGSL qualified Surface Cache shadow + Lambert radiance production.
// Texture reads use textureLoad; WebGPU does not require storage-image reads.
struct Card {center:vec4<f32>,extent:vec4<f32>,placement:vec4<u32>,owner:vec4<u32>}
struct Dispatch {range:vec4<u32>,address:vec4<u32>}
struct ShadowParams {sdfOrigin:vec4<f32>,sdfExtent:vec4<f32>,lightToward:vec4<f32>,policy:vec4<f32>,counts:vec4<u32>,matrices:array<mat4x4<f32>,4>,shadowMeta:vec4<f32>}
struct LightParams {counts:vec4<u32>,grid:vec4<f32>,probes:vec4<u32>,gates:vec4<u32>,policy:vec4<f32>}
struct Light {position:vec4<f32>,color:vec4<f32>,direction:vec4<f32>}
@group(0) @binding(0) var<storage,read> cards:array<Card>;
@group(0) @binding(1) var<storage,read> dispatches:array<Dispatch>;
@group(0) @binding(2) var<uniform> shadow:ShadowParams;
@group(0) @binding(3) var sdf:texture_3d<f32>;
@group(0) @binding(4) var depthAtlas:texture_2d<f32>;
@group(0) @binding(5) var normalAtlas:texture_2d<f32>;
@group(0) @binding(6) var shadow0:texture_depth_2d;
@group(0) @binding(7) var shadow1:texture_depth_2d;
@group(0) @binding(8) var shadow2:texture_depth_2d;
@group(0) @binding(9) var shadow3:texture_depth_2d;
@group(0) @binding(10) var<storage,read_write> visibility:array<f32>;
@group(0) @binding(11) var<uniform> params:LightParams;
@group(0) @binding(12) var<storage,read> lights:array<Light>;
@group(0) @binding(13) var albedoAtlas:texture_2d<f32>;
@group(0) @binding(14) var emissiveAtlas:texture_2d<f32>;
@group(0) @binding(15) var previousLighting:texture_2d<f32>;
@group(0) @binding(16) var indirectAtlas:texture_2d<f32>;
@group(0) @binding(17) var<storage,read> irradiance:array<vec4<f32>>;
@group(0) @binding(18) var<storage,read> probeDepth:array<f32>;
@group(0) @binding(19) var<storage,read> probeSky:array<f32>;
@group(0) @binding(20) var lightingOut:texture_storage_2d<rgba16float,write>;
@group(0) @binding(21) var nodirectOut:texture_storage_2d<rgba16float,write>;
fn finite(v:f32)->bool{return (bitcast<u32>(v)&0x7f800000u)!=0x7f800000u;}
fn finite3(v:vec3<f32>)->bool{return finite(v.x)&&finite(v.y)&&finite(v.z);}
fn octDecode(uv:vec2<f32>)->vec3<f32>{
 let f=uv*2.0-1.0;var n=vec3<f32>(f,1.0-abs(f.x)-abs(f.y));
 if(n.z<0.0){n=vec3<f32>((1.0-abs(n.yx))*select(vec2<f32>(-1),vec2<f32>(1),n.xy>=vec2<f32>(0)),n.z);}
 return normalize(n);
}
fn octEncode(d:vec3<f32>)->vec2<f32>{var u=d.xy/(abs(d.x)+abs(d.y)+abs(d.z));if(d.z<0.0){u=(1.0-abs(u.yx))*select(vec2<f32>(-1),vec2<f32>(1),u>=vec2<f32>(0));}return u*.5+.5;}
fn cardWorld(pixel:vec2<u32>,depth:f32,c:Card)->vec3<f32>{
 let uv=(vec2<f32>(pixel-c.placement.zw)+.5)/f32(c.placement.y&65535u)-.5;
 let axis=c.placement.x&255u;let sign=select(-1.0,1.0,((c.placement.x>>8u)&255u)!=0u);
 if(axis==0u){return c.center.xyz+vec3<f32>(sign*depth,uv.x*c.extent.y*2.0,uv.y*c.extent.z*2.0);}
 if(axis==1u){return c.center.xyz+vec3<f32>(uv.x*c.extent.x*2.0,sign*depth,uv.y*c.extent.z*2.0);}
 return c.center.xyz+vec3<f32>(uv.x*c.extent.x*2.0,uv.y*c.extent.y*2.0,sign*depth);
}
fn findCard(tid:u32,count:u32)->u32{
 var lo=0u;var hi=count;while(lo<hi){let mid=(lo+hi)>>1u;let d=dispatches[mid];if(d.range.x+d.range.y<=tid){lo=mid+1u;}else{hi=mid;}}return lo;
}
fn atlasPixel(tid:u32,d:Dispatch)->vec2<u32>{let local=tid-d.range.x;return vec2<u32>(d.range.w+local%d.range.z,d.address.x+local/d.range.z);}
fn pcf(map:texture_depth_2d,uv:vec2<f32>,z:f32)->f32{
 let dims=vec2<i32>(textureDimensions(map));let q=clamp(uv,vec2<f32>(0),vec2<f32>(1))*vec2<f32>(dims)-.5;
 let base=vec2<i32>(floor(q));let a=max(base,vec2<i32>(0));let b=min(base+1,dims-1);
 var v=select(1.0,0.0,z-.0015>textureLoad(map,a,0));v+=select(1.0,0.0,z-.0015>textureLoad(map,vec2<i32>(b.x,a.y),0));
 v+=select(1.0,0.0,z-.0015>textureLoad(map,vec2<i32>(a.x,b.y),0));v+=select(1.0,0.0,z-.0015>textureLoad(map,b,0));return v*.25;
}
fn mapVisibility(world:vec3<f32>,normal:vec3<f32>,footprint:f32)->f32{
 let p=world+normal*(footprint*shadow.shadowMeta.y);
 for(var c=0u;c<min(u32(shadow.shadowMeta.x+.5),4u);c++){
  let clip=shadow.matrices[c]*vec4<f32>(p,1);if(clip.w<=1e-6){continue;}let ndc=clip.xyz/clip.w;
  if(any(ndc.xy<vec2<f32>(-1))||any(ndc.xy>vec2<f32>(1))||ndc.z<0.0||ndc.z>1.0){continue;}
  let uv=vec2<f32>(ndc.x*.5+.5,.5-ndc.y*.5);
  switch c{case 0u:{return pcf(shadow0,uv,ndc.z);}case 1u:{return pcf(shadow1,uv,ndc.z);}case 2u:{return pcf(shadow2,uv,ndc.z);}default:{return pcf(shadow3,uv,ndc.z);}}
 }return -1.0;
}
fn sampleSdf(p:vec3<f32>)->f32{
 let local=p-shadow.sdfOrigin.xyz;if(shadow.counts.w==0u||any(local<vec3<f32>(0))||any(local>=shadow.sdfExtent.xyz)){return 1e10;}
 let res=i32(shadow.sdfOrigin.w);let t=clamp(vec3<i32>(local/shadow.sdfExtent.xyz*f32(res)),vec3<i32>(0),vec3<i32>(res-1));return textureLoad(sdf,t,0).r;
}
fn sdfVisibility(world:vec3<f32>,normal:vec3<f32>)->f32{
 let voxel=max(shadow.sdfExtent.x/max(shadow.sdfOrigin.w,1.0),.01);let origin=world+normal*(voxel*shadow.policy.y);
 let threshold=min(voxel*.5,max(sampleSdf(origin)*.5,0.0));var t=voxel;
 for(var step=0u;step<16u;step++){
  let d=sampleSdf(origin+shadow.lightToward.xyz*t);if(d>1e9){break;}if(d<threshold){return 0.0;}
  t+=select(max(d,voxel*.25),d*1.2,d>threshold*4.0);if(t>=shadow.policy.x){break;}
 }return 1.0;
}
@compute @workgroup_size(256) fn surface_cache_shadow(@builtin(global_invocation_id) id:vec3<u32>){
 if(id.x>=shadow.counts.x){return;}let index=findCard(id.x,shadow.counts.y);if(index>=shadow.counts.y){return;}
 let d=dispatches[index];let c=cards[d.address.y];let pixel=atlasPixel(id.x,d);let at=pixel.x+shadow.counts.z*pixel.y;
 let depth=textureLoad(depthAtlas,vec2<i32>(pixel),0).r;if(depth<=0.0){visibility[at]=1.0;return;}
 let normal=octDecode(textureLoad(normalAtlas,vec2<i32>(pixel),0).rg);let world=cardWorld(pixel,depth,c);
 let mapped=mapVisibility(world,normal,length(c.extent.xyz)/f32(max(c.placement.y&65535u,1u)));
 if(mapped>=0.0){visibility[at]=mapped;}else{visibility[at]=sdfVisibility(world,normal);}
}
fn evalSH(p:u32,n:vec3<f32>)->vec3<f32>{
 let b=p*9u;let x=n.x;let y=n.y;let z=n.z;
 return irradiance[b].xyz*.28209479
  +irradiance[b+1u].xyz*(-.48860251*.6666667*y)+irradiance[b+2u].xyz*(.48860251*.6666667*z)+irradiance[b+3u].xyz*(-.48860251*.6666667*x)
  +irradiance[b+4u].xyz*(1.09254843*.25*x*y)+irradiance[b+5u].xyz*(-1.09254843*.25*y*z)+irradiance[b+6u].xyz*(.31539157*.25*(3.0*z*z-1.0))
  +irradiance[b+7u].xyz*(-1.09254843*.25*z*x)+irradiance[b+8u].xyz*(.54627421*.25*(x*x-y*y));
}
struct Gather {value:vec3<f32>,inVolume:bool}
fn gather(world:vec3<f32>,n:vec3<f32>)->Gather{
 var out:Gather;out.value=vec3<f32>(0);out.inVolume=false;if(params.probes.w==0u){return out;}
 let spacing=max(params.grid.w,.01);let gp=(world-params.grid.xyz)/spacing;let counts=vec3<i32>(params.probes.xyz);let limit=counts-1;
 if(any(gp<vec3<f32>(0))||any(gp>vec3<f32>(limit))){return out;}out.inVolume=true;
 let b0=clamp(vec3<i32>(floor(gp)),vec3<i32>(0),limit);let b1=min(b0+1,limit);let fr=clamp(fract(gp),vec3<f32>(0),vec3<f32>(1));
 var ids:array<u32,8>;var weights:array<f32,8>;var positions:array<vec3<i32>,8>;var pixOpen=0.0;var pixW=0.0;
 for(var i=0u;i<8u;i++){let bits=vec3<u32>(i&1u,(i>>1u)&1u,(i>>2u)&1u);let pc=select(b0,b1,bits!=vec3<u32>(0));positions[i]=pc;
  let w=select(1.0-fr,fr,bits!=vec3<u32>(0));weights[i]=w.x*w.y*w.z;ids[i]=u32(pc.x+counts.x*(pc.y+counts.y*pc.z));
  if(params.gates.w!=0u){pixOpen+=probeSky[ids[i]*2u]*weights[i];pixW+=weights[i];}}
 if(pixW>0.0){pixOpen/=pixW;}var accVis=vec3<f32>(0);var accLow=vec3<f32>(0);var visW=0.0;var lowW=0.0;
 for(var i=0u;i<8u;i++){let tw=weights[i];if(tw<=0.0){continue;}let p=ids[i];let irr=max(evalSH(p,n),vec3<f32>(0));
  let delta=params.grid.xyz+vec3<f32>(positions[i])*spacing-(world+n*spacing*.05);let distance=length(delta);let direction=delta/max(distance,1e-5);
  let uv=octEncode(-direction);let texel=clamp(vec2<u32>(uv*8.0),vec2<u32>(0),vec2<u32>(7));let oct=texel.y*8u+texel.x;let mean=probeDepth[p*128u+oct];let variance=probeDepth[p*128u+64u+oct];
  var inside=1.0;var burial=1.0;if(mean<spacing*.2){let f=smoothstep(0.0,.2,mean/spacing);inside=mix(.2,1.0,f);burial=mix(.1,1.0,f);}
  let nw=max(mix(.2,1.0,clamp((dot(n,direction)+.5)/.7,0.0,1.0)),.40);let dist=max(distance-mean,0.0);
  var vw=1.0;if(variance>=.0001){vw=clamp(max(variance/(variance+dist*dist)*2.0,.2),0.0,1.0);}vw=max(vw,.38);
  var classify=1.0;if(params.gates.w!=0u){let skyness=smoothstep(.15,.45,probeSky[p*2u]);let enclosed=1.0-smoothstep(.05,.25,pixOpen);classify=1.0-skyness*enclosed*.85;if(params.counts.w!=0u){classify=mix(1.0,classify,probeSky[p*2u+1u]);}}
  let a=tw*nw*vw*inside*classify;let b=tw*burial*classify;accVis+=irr*a;visW+=a;accLow+=irr*b;lowW+=b;
 }
 if(visW>0.0){out.value+=accVis/visW*.45;}if(lowW>0.0){out.value+=accLow/lowW*.55;}
 out.value=min(out.value*3.14159265,vec3<f32>(64));return out;
}
@compute @workgroup_size(256) fn surface_cache_light_eval(@builtin(global_invocation_id) id:vec3<u32>){
 if(id.x>=params.counts.x){return;}let index=findCard(id.x,params.counts.y);if(index>=params.counts.y){return;}
 let d=dispatches[index];let c=cards[d.address.y];let pixel=atlasPixel(id.x,d);let at=vec2<i32>(pixel);
 let albedo=textureLoad(albedoAtlas,at,0);let depth=textureLoad(depthAtlas,at,0).r;let transport=u32(round(clamp(albedo.a,0.0,1.0)*255.0));
 if((transport&128u)==0u||!finite(depth)||depth<0.0||!finite3(albedo.rgb)||!finite(albedo.a)){textureStore(lightingOut,at,vec4<f32>(0));textureStore(nodirectOut,at,vec4<f32>(0));return;}
 let normal=octDecode(textureLoad(normalAtlas,at,0).rg);let world=cardWorld(pixel,max(depth,0.0),c);let emissive=min(textureLoad(emissiveAtlas,at,0).rgb,vec3<f32>(8));
 let back=f32((transport>>3u)&15u)/15.0*exp(-2.0*f32(transport&7u)/7.0);var direct=vec3<f32>(0);
 for(var i=0u;i<min(params.counts.z,256u);i++){
  let light=lights[i];var l:vec3<f32>;var attenuation:f32;
  if(u32(light.direction.w)==1u){l=-normalize(light.direction.xyz);attenuation=visibility[pixel.x+shadow.counts.z*pixel.y];}
  else{let delta=light.position.xyz-world;let dist=length(delta);let radius=light.position.w;if(dist>radius){continue;}l=delta/max(dist,.001);attenuation=max(1.0-dist*dist/(radius*radius),0.0);}
  let cosine=dot(normal,l);direct+=light.color.xyz*(max(cosine,0.0)+max(-cosine,0.0)*back)*attenuation;
 }
 var prev=textureLoad(previousLighting,at,0).rgb;if(!finite3(prev)){prev=vec3<f32>(0);}prev=clamp(prev,vec3<f32>(0),vec3<f32>(256));
 let incoming=gather(world,normal);let indirect=incoming.value*params.policy.x;var tile=vec3<f32>(0);
 if(params.gates.x!=0u){let sample=textureLoad(indirectAtlas,at,0);if(sample.a>0.0){tile=sample.rgb;}}
 var weight=.25;if(params.gates.y!=0u){weight=bitcast<f32>(params.gates.y);}
 var bounce=tile+prev*albedo.rgb*.31830988;
 if(params.policy.y!=0.0||(params.probes.w!=0u&&incoming.inVolume)){bounce=tile*weight;}
 let result=min((direct+indirect)*albedo.rgb*.31830988+bounce+emissive,vec3<f32>(64));
 textureStore(lightingOut,at,vec4<f32>(result,1));textureStore(nodirectOut,at,vec4<f32>(clamp(result-direct*albedo.rgb*.31830988,vec3<f32>(0),vec3<f32>(64)),1));
}
// Each new slot starts explicitly black, including pixels outside card bounds.
@compute @workgroup_size(8,8) fn surface_cache_clear(@builtin(global_invocation_id) id:vec3<u32>){
 let size=textureDimensions(lightingOut);if(any(id.xy>=size)){return;}textureStore(lightingOut,vec2<i32>(id.xy),vec4<f32>(0));textureStore(nodirectOut,vec2<i32>(id.xy),vec4<f32>(0));
}
