// Native WGSL measured geometry Finalize. No nearby-owner, analytic direct or
// previous-screen fallback. Unknown radiance never changes geometric distance.
struct Ray {origin:vec4<f32>,direction:vec4<f32>}
struct Hit {distanceNormal:vec4<f32>,identity:vec4<u32>,a:vec4<f32>,e1:vec4<f32>,e2:vec4<f32>,measured:vec4<f32>}
struct Card {center:vec4<f32>,extent:vec4<f32>,placement:vec4<u32>,owner:vec4<u32>}
struct Directory {lo:vec4<f32>,hi:vec4<f32>,range:vec4<u32>}
struct Params {counts:vec4<u32>,sky:vec4<f32>}
@group(0) @binding(0) var<storage,read> rays:array<Ray>;
@group(0) @binding(1) var<storage,read> hits:array<Hit>;
@group(0) @binding(2) var<storage,read> cards:array<Card>;
@group(0) @binding(3) var<storage,read> directory:array<Directory>;
@group(0) @binding(4) var<storage,read_write> result:array<vec4<f32>>;
// status: 0 unknown, 1 qualified card (black valid), 2 certified sky,
// 3 hit with unknown radiance, 4 bounded miss. y selected card, z key, w reads.
@group(0) @binding(5) var<storage,read_write> audit:array<vec4<u32>>;
@group(0) @binding(6) var<uniform> params:Params;
@group(0) @binding(7) var depthAtlas:texture_2d<f32>;
@group(0) @binding(8) var lightingAtlas:texture_2d<f32>;
fn finite(v:f32)->bool{return (bitcast<u32>(v)&0x7f800000u)!=0x7f800000u;}
fn finite3(v:vec3<f32>)->bool{return finite(v.x)&&finite(v.y)&&finite(v.z);}
fn finite4(v:vec4<f32>)->bool{return finite3(v.xyz)&&finite(v.w);}
struct Candidate {rgb:vec3<f32>,key:f32,valid:bool}
fn candidate(c:Card,owner:u32,pos:vec3<f32>,normal:vec3<f32>,ray:vec3<f32>)->Candidate {
 var out:Candidate;out.key=1e30;
 let axis=c.placement.x&255u;let dir=(c.placement.x>>8u)&255u;let res=c.placement.y&65535u;
 if(c.owner.x!=owner||!finite3(pos)||!finite3(normal)||dot(normal,normal)<.25||dot(normal,ray)>=0.0||res==0u||axis>2u||dir>1u){return out;}
 let n=normalize(normal);let sign=select(-1.0,1.0,dir!=0u);var face=vec3<f32>(0);face[axis]=sign;
 if(dot(face,n)>-.5||dot(face,-ray)>=0.0){return out;}
 let local=pos-c.center.xyz;var plane:vec2<f32>;var ext:vec2<f32>;
 if(axis==0u){plane=local.yz;ext=c.extent.yz;}else if(axis==1u){plane=local.xz;ext=c.extent.xz;}else{plane=local.xy;ext=c.extent.xy;}
 let uv=plane/(ext*2.0)+.5;
 if(local[axis]*sign<0.0||!finite3(vec3<f32>(uv,0))||any(uv<vec2<f32>(0))||any(uv>=vec2<f32>(1))){return out;}
 let size=textureDimensions(depthAtlas);let offset=c.placement.zw;
 if(any(size==vec2<u32>(0))||any(size!=textureDimensions(lightingAtlas))||any(offset>=size)||any(vec2<u32>(res)>size-offset)){return out;}
 let pixel=vec2<u32>(clamp(vec2<f32>(offset)+clamp(uv,vec2<f32>(0),vec2<f32>(.999999))*f32(res),vec2<f32>(offset),vec2<f32>(offset+res-1u)));
 let depth=textureLoad(depthAtlas,vec2<i32>(pixel),0).r;let light=textureLoad(lightingAtlas,vec2<i32>(pixel),0);
 if(!finite(depth)||depth<0.0||!finite4(light)||light.a<=.5){return out;}
 let p=(vec2<f32>(pixel-offset)+.5)/f32(res)-.5;var localCaptured:vec3<f32>;
 if(axis==0u){localCaptured=vec3<f32>(sign*depth,p.x*c.extent.y*2.0,p.y*c.extent.z*2.0);}
 else if(axis==1u){localCaptured=vec3<f32>(p.x*c.extent.x*2.0,sign*depth,p.y*c.extent.z*2.0);}
 else{localCaptured=vec3<f32>(p.x*c.extent.x*2.0,p.y*c.extent.y*2.0,sign*depth);}
 let captured=c.center.xyz+localCaptured;let delta=captured-pos;let along=dot(delta,n);let lateral=length(delta-n*along);let footprint=2.0*length(c.extent.xyz)/f32(res);
 if(!finite3(captured)||abs(along)>.002||lateral>max(footprint,.002)){return out;}
 out.rgb=light.rgb;out.key=lateral+abs(along);out.valid=true;return out;
}
@compute @workgroup_size(64) fn measured_radiance(@builtin(global_invocation_id) id:vec3<u32>){
 let i=id.x;if(i>=params.counts.x||i>=arrayLength(&result)||i>=arrayLength(&audit)){return;}
 var value=vec4<f32>(0,0,0,-1);var info=vec4<u32>(0,0xffffffffu,bitcast<u32>(1e30),0);
 if(i>=arrayLength(&rays)||i>=arrayLength(&hits)||params.counts.y>arrayLength(&cards)||params.counts.z>arrayLength(&directory)||params.counts.w!=1u){result[i]=value;audit[i]=info;return;}
 let ray=rays[i];let hit=hits[i];let d=hit.measured.x;
 if(finite(d)){value.w=d;}
 if(!finite4(ray.origin)||!finite4(ray.direction)||ray.origin.w!=0.0||dot(ray.direction.xyz,ray.direction.xyz)<.25){result[i]=value;audit[i]=info;return;}
 if(d>=0.0&&finite(d)){
  info.x=3u;
  let tag=hit.identity.w;let owner=hit.identity.y;
  if(hit.identity.x==1u&&tag>=2u&&tag<=65537u&&tag==owner+2u&&owner<params.counts.z&&finite3(hit.measured.yzw)&&dot(hit.measured.yzw,hit.measured.yzw)>.25){
   let range=directory[owner].range.zw;
   if(range.x<=range.y&&range.y<=params.counts.y){
    let pos=ray.origin.xyz+ray.direction.xyz*d;var best=1e30;
    for(var at=range.x;at<range.y&&info.w<16u;at++){
     if(at>=4096u){continue;}let card=cards[at];if(card.owner.x!=owner){continue;}info.w++;
     let sample=candidate(card,owner,pos,hit.measured.yzw,ray.direction.xyz);
     if(sample.valid&&sample.key<best){best=sample.key;value=vec4<f32>(sample.rgb,d);info.x=1u;info.y=at;info.z=bitcast<u32>(sample.key);}
    }
   }
  }
 }else if(d==-2.0&&hit.identity.x==0u){
  let up=clamp(ray.direction.y,0.0,1.0);let lower=select(1.0,1.0-clamp(params.sky.w,0.0,1.0),ray.direction.y<0.0);
  value=vec4<f32>(mix(vec3<f32>(4.40,4.20,3.80),vec3<f32>(5.50,5.76,6.00),pow(up,.45))*params.sky.xyz*lower,-2);info.x=2u;
 }else if(d==-3.0){info.x=4u;}
 result[i]=value;audit[i]=info;
}
