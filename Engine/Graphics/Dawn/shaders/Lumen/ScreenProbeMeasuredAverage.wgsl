// Native frozen-profile SH projection. Tier compaction and SH2 diagnostics
// are deliberately outside this profile. Sampling must match MeasuredCommon.
struct AverageData {
    totalProbes:u32,raysPerProbe:u32,frameSeed:f32,isMode:f32,
    tier3Mode:u32,tierRaysMode:u32,sh2CheckMode:u32,pad0:u32,
    skyConfFloor:f32,pad1:f32,pad2:f32,pad3:f32,
}
@group(0) @binding(0) var<storage,read> radiance:array<vec4<f32>>;
@group(0) @binding(1) var<storage,read_write> outputSH:array<vec4<f32>>;
@group(0) @binding(2) var<uniform> avg:AverageData;
@group(0) @binding(3) var<storage,read> normals:array<vec4<f32>>;
@group(0) @binding(4) var<storage,read> importanceSH:array<vec4<f32>>;
@group(0) @binding(7) var<storage,read_write> openSky:array<vec4<f32>>;
fn spHash(value:u32)->u32 {
    var v=value;v=v^(v>>16u);v*=0x7feb352du;v=v^(v>>15u);v*=0x846ca68bu;return v^(v>>16u);
}
fn spOffset(probe:u32,seed:f32)->vec2<f32> {
    if(seed<0.0){return vec2<f32>(0);}let h=spHash(u32(seed)^(probe*0x9e3779b9u));
    return vec2<f32>(f32(h&0xffffu),f32((h>>16u)&0xffffu))*(2.0/65535.0)-1.0;
}
fn spRotation(probe:u32,seed:f32)->mat3x3<f32> {
    if(seed<0.0){return mat3x3<f32>(vec3<f32>(1,0,0),vec3<f32>(0,1,0),vec3<f32>(0,0,1));}
    let h1=spHash((probe*0x9e3779b9u)^u32(seed));let h2=spHash(h1^0x68bc21ebu);
    let a1=f32(h1)*(2.0*3.141592653589793/4294967295.0);let a2=f32(h2)*(2.0*3.141592653589793/4294967295.0);
    let c1=cos(a1);let s1=sin(a1);let c2=cos(a2);let s2=sin(a2);
    let rz1=mat3x3<f32>(vec3<f32>(c1,s1,0),vec3<f32>(-s1,c1,0),vec3<f32>(0,0,1));
    let rx=mat3x3<f32>(vec3<f32>(1,0,0),vec3<f32>(0,0,-1),vec3<f32>(0,1,0));
    let rz2=mat3x3<f32>(vec3<f32>(c2,s2,0),vec3<f32>(-s2,c2,0),vec3<f32>(0,0,1));return rz2*rx*rz1;
}
fn spFibonacci(ray:u32,count:u32)->vec3<f32> {
    let theta=2.0*3.141592653589793*f32(ray)/1.618033988749895;
    let phi=acos(1.0-2.0*(f32(ray)+.5)/f32(count));return vec3<f32>(sin(phi)*cos(theta),sin(phi)*sin(theta),cos(phi));
}
fn spTangent(n:vec3<f32>)->vec3<f32> {
    let up=select(vec3<f32>(1,0,0),vec3<f32>(0,1,0),abs(n.y)<.999);return normalize(cross(up,n));
}
fn spOrient(d:vec3<f32>,n:vec3<f32>)->vec3<f32> {let t=spTangent(n);return normalize(t*d.x+cross(n,t)*d.y+n*d.z);}
// Return local direction and 1/(N*q). A 25% uniform mixture keeps every
// candidate reachable; the PDF is evaluated against the actual history SH.
fn spImportance(ray:u32,count:u32,probe:u32,seed:f32,n:vec3<f32>,sh:array<vec4<f32>,4>)->vec4<f32> {
    let total=min(count,64u);let rot=spRotation(probe,seed);let tangent=spTangent(n);let bitangent=cross(n,tangent);
    var pdf:array<f32,64>;var sum=0.0;
    for(var i=0u;i<total;i++){
        let local=rot*spFibonacci(i,total);let world=tangent*local.x+bitangent*local.y+n*local.z;
        let radiance=sh[0].xyz*.282095+sh[1].xyz*(-.488603*world.y)+sh[2].xyz*(.488603*world.z)+sh[3].xyz*(-.488603*world.x);
        let lum=max(dot(radiance,vec3<f32>(.2126,.7152,.0722)),0.0);pdf[i]=max(local.z,.02)*(lum+.05);sum+=pdf[i];
    }
    for(var i=0u;i<total;i++){pdf[i]=.25/f32(total)+.75*pdf[i]/max(sum,1e-6);}
    let seedU=u32(max(seed,0.0));let h=spHash((probe*0x9e3779b9u)^seedU^(ray*0x85ebca6bu));
    let jitter=(f32(h&0xffffffu)+.5)/16777216.0;let u=(f32(ray)+jitter)/f32(total);
    var chosen=total-1u;var acc=0.0;for(var i=0u;i<total;i++){acc+=pdf[i];if(u<=acc){chosen=i;break;}}
    return vec4<f32>(rot*spFibonacci(chosen,total),1.0/(f32(total)*max(pdf[chosen],1e-6)));
}

var<workgroup> sh:array<array<vec3<f32>,64>,4>;
var<workgroup> weights:array<f32,64>;
var<workgroup> luminance:array<f32,64>;
var<workgroup> luminanceSq:array<f32,64>;
var<workgroup> hits:array<f32,64>;
var<workgroup> participants:array<f32,64>;
@compute @workgroup_size(64)
fn measured_screen_average(@builtin(local_invocation_id) lid:vec3<u32>,@builtin(workgroup_id) group:vec3<u32>){
    let probe=group.x;let lane=lid.x;if(probe>=avg.totalProbes){return;}
    var ray=vec4<f32>(0);if(lane<avg.raysPerProbe){ray=radiance[probe*avg.raysPerProbe+lane];}
    // Every lane reaches every barrier, including inactive and absent rays.
    for(var b=0u;b<4u;b++){sh[b][lane]=vec3<f32>(0);}
    weights[lane]=0;luminance[lane]=0;luminanceSq[lane]=0;hits[lane]=0;participants[lane]=0;
    if(ray.w!=0.0){
        let initialLum=dot(ray.xyz,vec3<f32>(.2126,.7152,.0722));
        if(initialLum>64.0){ray=vec4<f32>(ray.xyz*(64.0/initialLum),ray.w);}
        let n=normalize(normals[probe].xyz);var direction=spRotation(probe,avg.frameSeed)*spFibonacci(lane,avg.raysPerProbe);var invNq=1.0;
        if(avg.isMode>.5){let base=probe*4u;let history=array<vec4<f32>,4>(importanceSH[base],importanceSH[base+1u],importanceSH[base+2u],importanceSH[base+3u]);let sample=spImportance(lane,avg.raysPerProbe,probe,avg.frameSeed,n,history);direction=sample.xyz;invNq=sample.w;}
        direction=spOrient(direction,n);let basis=vec4<f32>(.282095,-.488603*direction.y,.488603*direction.z,-.488603*direction.x);
        let weighted=ray.xyz*invNq;
        for(var b=0u;b<4u;b++){sh[b][lane]=weighted*basis[b];}
        weights[lane]=abs(ray.w);let lum=dot(ray.xyz,vec3<f32>(.2126,.7152,.0722));luminance[lane]=lum;luminanceSq[lane]=lum*lum;
        hits[lane]=select(0.0,1.0,ray.w>0.0);participants[lane]=1.0;
    }
    workgroupBarrier();
    for(var stride=32u;stride>0u;stride>>=1u){
        if(lane<stride){for(var b=0u;b<4u;b++){sh[b][lane]+=sh[b][lane+stride];}
            weights[lane]+=weights[lane+stride];luminance[lane]+=luminance[lane+stride];luminanceSq[lane]+=luminanceSq[lane+stride];hits[lane]+=hits[lane+stride];participants[lane]+=participants[lane+stride];}
        workgroupBarrier();
    }
    if(lane==0u){
        let base=probe*4u;for(var b=0u;b<4u;b++){outputSH[base+b]=vec4<f32>(0);}
        if(weights[0]>0.0&&participants[0]>0.0){
            let scale=12.566370614/f32(avg.raysPerProbe);let mean=luminance[0]/participants[0];let variance=max(luminanceSq[0]/participants[0]-mean*mean,0.0);
            let consistency=mix(.55,1.0,exp(-variance*4.0));let confidence=mix(avg.skyConfFloor,1.0,hits[0]/f32(avg.raysPerProbe))*consistency;
            outputSH[base]=vec4<f32>(sh[0][0]*scale,confidence);
            for(var b=1u;b<4u;b++){outputSH[base+b]=vec4<f32>(sh[b][0]*scale,1.0);}
        }
        let denom=max(participants[0],1.0);openSky[probe]=vec4<f32>(max(participants[0]-hits[0],0.0)/denom,hits[0]/denom,0,1);
    }
}
