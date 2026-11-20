// Native WGSL source shared by the frozen-profile SPGI stages.
struct ScreenProbeData {
    viewProjection:mat4x4<f32>,inverseVP:mat4x4<f32>,camera:vec4<f32>,
    grid:vec4<f32>,trace:vec4<f32>,sdfOrigins:array<vec4<f32>,3>,
    sdfVoxels:array<vec4<f32>,3>,sdfExtents:array<vec4<f32>,3>,sdfResolutions:vec4<f32>,
    surfaceCache:vec4<f32>,ddgi:array<vec4<f32>,2>,hzb:vec4<f32>,cache:vec4<f32>,
    diagnostics:vec4<f32>,hint:vec4<f32>,sky:vec4<f32>,wrc:array<vec4<f32>,2>,
    representative:vec4<f32>,tier:vec4<f32>,tierCounts:vec4<f32>,
}
@group(0) @binding(0) var<uniform> sp:ScreenProbeData;
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
