// Native WGSL mode-7 ray generation. Query results remain geometric evidence;
// surface qualification and radiance are separate downstream producers.
struct GeometryRay { origin_near:vec4<f32>, direction_far:vec4<f32> }
struct RayParameters { counts:vec4<u32>, distance:vec4<f32> }
@group(0) @binding(0) var<storage,read> probeOrigins:array<vec4<f32>>;
@group(0) @binding(1) var<storage,read> updateIndices:array<u32>;
@group(0) @binding(2) var<storage,read_write> measuredRays:array<GeometryRay>;
@group(0) @binding(3) var<uniform> rayParameters:RayParameters;
fn octHash(input:u32)->u32 {
    var x=input;x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;return x^(x>>16u);
}
fn octDirection(ray:u32,count:u32,frame:u32)->vec3<f32> {
    let cell=ray/(count/64u);
    let seed=octHash(frame^0x9e3779b9u)^octHash(ray^0x85ebca6bu);
    let jitter=(vec2<f32>(f32(octHash(seed^0x68bc21ebu)>>14u),f32(octHash(seed^0x02e5be93u)>>14u))+0.5)/262144.0;
    let oct=(vec2<f32>(f32(cell%8u),f32(cell/8u))+jitter)/8.0*2.0-1.0;
    var q=vec3<f32>(oct,1.0-abs(oct.x)-abs(oct.y));
    if(q.z<0.0){q=vec3<f32>((1.0-abs(q.yx))*select(vec2<f32>(-1.0),vec2<f32>(1.0),q.xy>=vec2<f32>(0.0)),q.z);}
    return normalize(q);
}
@compute @workgroup_size(64)
fn ddgi_measured_rays(@builtin(global_invocation_id) id:vec3<u32>){
    let c=rayParameters.counts;
    if(c.y<64u || c.y%64u!=0u || id.x>=arrayLength(&measuredRays)){return;}
    let update=id.x/c.y;let ray=id.x%c.y;
    if(update>=c.x || update>=arrayLength(&updateIndices)){return;}
    let probe=updateIndices[update];
    // A zero direction is explicitly invalid to the query. Avoid a NaN
    // constant: WGSL constant evaluation must not manufacture nonfinite values.
    var origin=vec3<f32>(0.0);var direction=vec3<f32>(0.0);
    if(probe<arrayLength(&probeOrigins) && c.w==1u){origin=probeOrigins[probe].xyz;direction=octDirection(ray,c.y,c.z);}
    measuredRays[id.x]=GeometryRay(vec4<f32>(origin,0.0),vec4<f32>(direction,rayParameters.distance.x));
}
