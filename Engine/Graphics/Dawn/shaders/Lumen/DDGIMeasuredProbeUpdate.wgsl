// Native mode7 probe updates. Compact measured ray order is mapped by updates.
// SH uses the actual traced directions; PDF comes from the frozen oct sampler.
struct Ray {origin:vec4<f32>,direction:vec4<f32>}
struct Stats {confidence:vec4<f32>,sky:vec4<f32>}
struct Params {counts:vec4<u32>,policy:vec4<f32>}
@group(0) @binding(0) var<uniform> params:Params; // probes, updates, rays/probe, frame; maxDistance, voxel, SHalpha, depthAlpha
@group(0) @binding(1) var<storage,read> radiance:array<vec4<f32>>;
@group(0) @binding(2) var<storage,read> rays:array<Ray>;
@group(0) @binding(3) var<storage,read> updates:array<u32>;
@group(0) @binding(4) var<storage,read> shHistory:array<vec4<f32>>;
@group(0) @binding(5) var<storage,read_write> shOut:array<vec4<f32>>;
@group(0) @binding(6) var<storage,read> depthHistory:array<f32>;
@group(0) @binding(7) var<storage,read_write> depthOut:array<f32>;
@group(0) @binding(8) var<storage,read_write> stats:array<Stats>;
fn finite(x:f32)->bool{return (bitcast<u32>(x)&0x7f800000u)!=0x7f800000u;}
fn finite3(x:vec3<f32>)->bool{return finite(x.x)&&finite(x.y)&&finite(x.z);}
fn hash(x0:u32)->u32{var x=x0;x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;return x^(x>>16u);}
fn pdf(ray:u32)->f32{
 let cell=ray/(params.counts.z/64u);let seed=hash(params.counts.w^0x9e3779b9u)^hash(ray^0x85ebca6bu);
 let jitter=(vec2<f32>(f32(hash(seed^0x68bc21ebu)>>14u),f32(hash(seed^0x02e5be93u)>>14u))+.5)/262144.0;
 let oct=(vec2<f32>(f32(cell%8u),f32(cell/8u))+jitter)/8.0*2.0-1.0;var q=vec3<f32>(oct,1.0-abs(oct.x)-abs(oct.y));
 if(q.z<0.0){q=vec3<f32>((1.0-abs(q.yx))*select(vec2<f32>(-1),vec2<f32>(1),q.xy>=vec2<f32>(0)),q.z);}
 let len=length(q);return len*len*len*.25;
}
fn octEncode(d:vec3<f32>)->vec2<f32>{var xy=d.xy/(abs(d.x)+abs(d.y)+abs(d.z));if(d.z<0.0){xy=(1.0-abs(xy.yx))*select(vec2<f32>(-1),vec2<f32>(1),xy>=vec2<f32>(0));}return xy*.5+.5;}
var<workgroup> partial:array<vec4<f32>,576>;
var<workgroup> energy:array<vec4<f32>,64>; // sky energy,total energy,hit count,escape count
@compute @workgroup_size(64) fn measured_update_sh(@builtin(local_invocation_index) lane:u32,@builtin(workgroup_id) group:vec3<u32>){
 let probe=updates[group.x];if(probe>=params.counts.x){return;}let count=params.counts.z;let base=group.x*count;
 for(var j=0u;j<9u;j++){partial[lane*9u+j]=vec4<f32>(0);}energy[lane]=vec4<f32>(0);
 for(var r=lane;r<count;r+=64u){let sample=radiance[base+r];var value=sample.xyz;let distance=sample.w;
  if(distance==-2.0){let e=dot(value,vec3<f32>(.2126,.7152,.0722));energy[lane]+=vec4<f32>(e,e,0,1);}
  else if(distance>=0.0){let weight=max(1.0-smoothstep(params.policy.x*.8,params.policy.x,distance),.2);value*=weight;value*=smoothstep(0.0,params.policy.y*2.0,distance);energy[lane]+=vec4<f32>(0,dot(value,vec3<f32>(.2126,.7152,.0722)),1,0);}
  let d=rays[base+r].direction.xyz;let x=d.x;let y=d.y;let z=d.z;
  let basis=array<f32,9>(.282095,-.488603*y,.488603*z,-.488603*x,1.092548*y*x,-1.092548*y*z,.315392*(3.0*z*z-1.0),-1.092548*x*z,.546274*(x*x-y*y));
  let weight=1.0/(f32(count)*max(pdf(r),1e-5));for(var j=0u;j<9u;j++){partial[lane*9u+j]+=vec4<f32>(value*basis[j]*weight,0);}
 }
 workgroupBarrier();if(lane!=0u){return;}
 var sum:array<vec3<f32>,9>;var total=vec4<f32>(0);
 for(var i=0u;i<64u;i++){for(var j=0u;j<9u;j++){sum[j]+=partial[i*9u+j].xyz;}total+=energy[i];}
 let old=stats[probe];let convergence=old.confidence.w;let settled=clamp(params.policy.z,0.0,1.0);
 var alpha=select(mix(.35,settled,min(convergence*2.0,1.0)),1.0,convergence<=0.0);
 let l1=max(min(length(sum[0])*3.0,192.0),.1);let l2=max(min(length(sum[0])*2.0,128.0),.05);
 var histNorm=0.0;var currentNorm=0.0;var diffNorm=0.0;
 for(var j=0u;j<9u;j++){var history=shHistory[probe*9u+j].xyz;if(!finite3(history)){history=vec3<f32>(0);}
  var value=mix(history,sum[j],alpha);let limit=select(select(l2,l1,j<4u),64.0,j==0u);let magnitude=length(value);if(magnitude>limit){value*=limit/magnitude;}
  histNorm+=length(history);currentNorm+=length(value);diffNorm+=length(value-history);shOut[probe*9u+j]=vec4<f32>(value,0);
 }
 let stability=clamp(1.0-diffNorm/max(max(histNorm,currentNorm),.001),0.0,1.0);
 let escape=total.w/f32(count);let first=old.sky.x<=0.0&&escape>0.0&&convergence<.5;
 var sky=old.sky;sky.x=select(mix(old.sky.x,escape,.35),escape,first);
 if(total.y>1e-6){let ratio=clamp(total.x/total.y,0.0,1.0);sky.y=select(mix(old.sky.y,ratio,.35),ratio,first);}
 stats[probe]=Stats(vec4<f32>(old.confidence.xyz,min(convergence+.0333,1.0)),sky);
}
var<workgroup> depthRays:array<vec4<f32>,256>; // distance,PDF weight,bin,valid
@compute @workgroup_size(64) fn measured_update_depth(@builtin(local_invocation_index) lane:u32,@builtin(workgroup_id) group:vec3<u32>){
 let probe=updates[group.x];if(probe>=params.counts.x){return;}let count=params.counts.z;let base=group.x*count;
 for(var r=lane;r<count;r+=64u){var d=radiance[base+r].w;depthRays[r]=vec4<f32>(0);if(d==-2.0||d==-3.0){d=params.policy.x;}
  if(d>=0.0&&finite(d)){let uv=octEncode(rays[base+r].direction.xyz);let pixel=clamp(vec2<u32>(uv*8.0),vec2<u32>(0),vec2<u32>(7));depthRays[r]=vec4<f32>(d,.25/pdf(r),f32(pixel.y*8u+pixel.x),1);}
 }
 workgroupBarrier();var mean=0.0;var m2=0.0;var weight=0.0;var observations=0u;
 for(var r=0u;r<count;r++){let sample=depthRays[r];if(sample.w!=0.0&&u32(sample.z)==lane){let next=weight+sample.y;
  if(weight==0.0){mean=sample.x;}else{let delta=sample.x-mean;let nextMean=mean+delta*(sample.y/next);m2+=sample.y*delta*(sample.x-nextMean);mean=nextMean;}
  weight=next;observations++;
 }}
 let at=probe*128u+lane;
 if(observations==0u){depthOut[at]=-1.0;depthOut[at+64u]=0.0;return;}
 let variance=max(m2/weight,0.0);let oldMean=depthHistory[at];let oldVar=depthHistory[at+64u];let history=finite(oldMean)&&finite(oldVar)&&oldMean>=0.0&&oldVar>=0.0;
 let ramp=clamp(f32(max(params.counts.w,1u)-1u)/60.0,0.0,1.0);let alpha=mix(1.0,params.policy.w,ramp);
 depthOut[at]=select(mean,mix(oldMean,mean,alpha),history);depthOut[at+64u]=select(variance,mix(oldVar,variance,alpha),history);
}
