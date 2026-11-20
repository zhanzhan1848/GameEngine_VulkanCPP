// Composed with GeometryFirstHit's binding-free query body and bindings 5..8.
struct SpatialData {totalProbes:u32,gridWidth:u32,sigma:f32,flags:u32}
@group(0) @binding(0) var<storage,read> inputSH:array<vec4<f32>>;
@group(0) @binding(1) var<storage,read_write> outputSH:array<vec4<f32>>;
@group(0) @binding(2) var<storage,read> positions:array<vec4<f32>>;
@group(0) @binding(3) var<uniform> spatial:SpatialData;
@group(0) @binding(4) var<storage,read> normals:array<vec4<f32>>;
fn finite4(v:vec4<f32>)->bool{return geometryFinite3(v.xyz)&&geometryFinite(v.w);}
fn spatialConnection(center:u32,neighbor:u32,p:vec3<f32>,q:vec3<f32>)->bool{
    if((spatial.flags&2u)==0u){return false;}
    var n=normals[center].xyz;var m=normals[neighbor].xyz;
    if(!geometryFinite3(n)||!geometryFinite3(m)||dot(n,n)<.25||dot(m,m)<.25){return false;}
    n=normalize(n);m=normalize(m);if(dot(n,m)<.5){return false;}
    let a=p+n*.01;let b=q+m*.01;let delta=b-a;let lengthValue=length(delta);
    if(!geometryFinite(lengthValue)||lengthValue<=.002){return false;}
    let hit=geometryFirstHit(GeometryRay(vec4<f32>(a,.001),vec4<f32>(delta/lengthValue,lengthValue-.001)));
    return hit.identity.x==0u;
}
@compute @workgroup_size(64)
fn measured_screen_spatial(@builtin(global_invocation_id) gid:vec3<u32>){
    let probe=gid.x;if(probe>=spatial.totalProbes){return;}let base=probe*4u;let p=positions[probe];let measured=(spatial.flags&1u)!=0u;
    var valid=p.w>0.0&&inputSH[base].w>0.0;
    if(measured){valid=valid&&finite4(p);for(var b=0u;b<4u;b++){valid=valid&&finite4(inputSH[base+b]);}}
    if(!valid){for(var b=0u;b<4u;b++){outputSH[base+b]=vec4<f32>(0);}return;}
    var sum:array<vec3<f32>,4>;for(var b=0u;b<4u;b++){sum[b]=inputSH[base+b].xyz*2.0;}
    var weightSum=2.0;var confidence=inputSH[base].w*2.0;let inverseSigma=1.0/(2.0*spatial.sigma*spatial.sigma);
    let offsets=array<vec2<i32>,4>(vec2<i32>(-1,0),vec2<i32>(1,0),vec2<i32>(0,-1),vec2<i32>(0,1));let cell=vec2<i32>(i32(probe%spatial.gridWidth),i32(probe/spatial.gridWidth));
    for(var i=0u;i<4u;i++){
        let c=cell+offsets[i];if(any(c<vec2<i32>(0))||c.x>=i32(spatial.gridWidth)){continue;}let neighbor=u32(c.y)*spatial.gridWidth+u32(c.x);if(neighbor>=spatial.totalProbes){continue;}
        let q=positions[neighbor];let nb=neighbor*4u;if(q.w<=0.0||inputSH[nb].w<=0.0){continue;}
        if(measured){var good=finite4(q);for(var b=0u;b<4u;b++){good=good&&finite4(inputSH[nb+b]);}if(!good||!spatialConnection(probe,neighbor,p.xyz,q.xyz)){continue;}}
        let delta=p.xyz-q.xyz;let weight=exp(-dot(delta,delta)*inverseSigma);
        for(var b=0u;b<4u;b++){sum[b]+=inputSH[nb+b].xyz*weight;}confidence+=inputSH[nb].w*weight;weightSum+=weight;
    }
    let inverseWeight=1.0/weightSum;outputSH[base]=vec4<f32>(sum[0]*inverseWeight,confidence*inverseWeight);
    for(var b=1u;b<4u;b++){outputSH[base+b]=vec4<f32>(sum[b]*inverseWeight,1);}
}
