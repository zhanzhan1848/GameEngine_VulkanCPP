// Native frozen-profile SPGI Finalize, surface half. The fallback half is a
// separate dispatch so both stages fit WebGPU's eight storage-buffer limit.
// Callers must reject zero-hit retries, compacted tiers and non-measured mode.
// The bounded test harness enforces this; production history wiring is pending.
struct SpRay { originWeight:vec4<f32>, directionCosine:vec4<f32> }
struct SpCard { center:vec4<f32>, extent:vec4<f32>, placement:vec4<u32>, owner:vec4<u32> }
@group(0) @binding(1) var<storage,read> positions:array<vec4<f32>>;
@group(0) @binding(2) var<storage,read> rays:array<SpRay>;
@group(0) @binding(3) var<storage,read> hits:array<vec4<f32>>;
@group(0) @binding(4) var<storage,read> cards:array<SpCard>;
@group(0) @binding(5) var<storage,read> grid:array<u32>;
@group(0) @binding(6) var<storage,read> gridCards:array<u32>;
@group(0) @binding(7) var<storage,read_write> radiance:array<vec4<f32>>;
// status: 0 inactive, 1 HZB, 2 SC, 3 needs DDGI, 4 DDGI, 5 unsupported.
// y: selected card, z: best key bits, w: evaluated candidates.
@group(0) @binding(8) var<storage,read_write> audit:array<vec4<u32>>;
@group(0) @binding(9) var previousColor:texture_2d<f32>;
@group(0) @binding(10) var cardDepth:texture_2d<f32>;
@group(0) @binding(11) var cardLighting:texture_2d<f32>;
@group(0) @binding(12) var cardAlbedo:texture_2d<f32>;
fn spFinite(x:f32)->bool {return (bitcast<u32>(x)&0x7f800000u)!=0x7f800000u;}
fn spFinite3(x:vec3<f32>)->bool {return spFinite(x.x)&&spFinite(x.y)&&spFinite(x.z);}
fn spBilinear(uv:vec2<f32>)->vec3<f32> {
    let size=vec2<i32>(textureDimensions(previousColor));let px=uv*vec2<f32>(size)-.5;
    let base=vec2<i32>(floor(px));let f=fract(px);
    let a=textureLoad(previousColor,clamp(base,vec2<i32>(0),size-1),0).rgb;
    let b=textureLoad(previousColor,clamp(base+vec2<i32>(1,0),vec2<i32>(0),size-1),0).rgb;
    let c=textureLoad(previousColor,clamp(base+vec2<i32>(0,1),vec2<i32>(0),size-1),0).rgb;
    let d=textureLoad(previousColor,clamp(base+vec2<i32>(1,1),vec2<i32>(0),size-1),0).rgb;
    return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
}
fn spFirefly(value:vec3<f32>)->vec3<f32> {
    return value/(1.0+max(dot(value,vec3<f32>(.2126,.7152,.0722))-sp.tierCounts.y,0.0)*2.0);
}
fn spFace(c:SpCard)->vec3<f32> {
    var n=vec3<f32>(0);n[c.placement.x&255u]=select(-1.0,1.0,((c.placement.x>>8u)&255u)>0u);return n;
}
// xy atlas coordinates, z validity; projection expands discovery only.
fn spProject(c:SpCard,p:vec3<f32>,pad:f32)->vec3<f32> {
    let axis=c.placement.x&255u;let res=c.placement.y&65535u;
    let local=p-c.center.xyz;var plane=local.xy;var extent=c.extent.xy;
    if(axis==0u){plane=local.yz;extent=c.extent.yz;}else if(axis==1u){plane=local.xz;extent=c.extent.xz;}
    if(dot(local,spFace(c))<0.0){return vec3<f32>(0);}
    let uv=plane/(extent*2.0)+.5;let margin=1.0/f32(max(res,1u));
    let extra=select(vec2<f32>(0),vec2<f32>(pad)/(extent*2.0),extent>vec2<f32>(1e-4));
    if(any(uv< -vec2<f32>(margin)-extra)||any(uv>=vec2<f32>(1.0+margin)+extra)){return vec3<f32>(0);}
    return vec3<f32>(vec2<f32>(c.placement.zw)+clamp(uv,vec2<f32>(0),vec2<f32>(.999999))*f32(res),1);
}
fn spCaptured(c:SpCard,px:vec2<u32>,depth:f32)->vec3<f32> {
    let axis=c.placement.x&255u;let uv=(vec2<f32>(px-c.placement.zw)+.5)/f32(c.placement.y&65535u)-.5;
    let d=select(-depth,depth,((c.placement.x>>8u)&255u)>0u);var local=vec3<f32>(uv*c.extent.xy*2.0,d);
    if(axis==0u){local=vec3<f32>(d,uv.x*c.extent.y*2.0,uv.y*c.extent.z*2.0);}
    else if(axis==1u){local=vec3<f32>(uv.x*c.extent.x*2.0,d,uv.y*c.extent.z*2.0);}
    return c.center.xyz+local;
}
fn spHitNormal(packed:f32)->vec3<f32> {
    // Packed snorm payload may be a float NaN. Preserve its bits.
    let uv=clamp(unpack2x16snorm(bitcast<u32>(packed))*.5+.5,vec2<f32>(0),vec2<f32>(1));
    let f=uv*2.0-1.0;var n=vec3<f32>(f,1.0-abs(f.x)-abs(f.y));
    let t=clamp(-n.z,0.0,1.0);n=vec3<f32>(n.xy+select(vec2<f32>(t),vec2<f32>(-t),n.xy>=vec2<f32>(0)),n.z);
    return normalize(n);
}
struct SpCandidate { rgb:vec3<f32>, key:f32, accepted:bool }
fn spCandidate(c:SpCard,pos:vec3<f32>,direction:vec3<f32>,hint:vec3<f32>)->SpCandidate {
    var out:SpCandidate;out.key=1e30;
    let axis=c.placement.x&255u;let res=c.placement.y&65535u;let size=textureDimensions(cardDepth);
    if(axis>2u||res==0u||any(c.placement.zw>=size)||any(vec2<u32>(res)>size-c.placement.zw)){return out;}
    let tolerance=max(.15,sp.sdfVoxels[0].x*1.25);let projection=spProject(c,pos,min(tolerance,.45));
    if(projection.z==0.0){return out;}
    let pixel=vec2<i32>(clamp(projection.xy,vec2<f32>(0),vec2<f32>(c.placement.zw+res-1u)));
    let face=spFace(c);var facing= -direction;var attenuation=1.0;
    if(dot(face,facing)>=0.0){
        let bits=u32(round(clamp(textureLoad(cardAlbedo,pixel,0).a,0.0,1.0)*255.0));
        let transport=f32((bits>>3u)&15u)/15.0*exp(-2.0*f32(bits&7u)/7.0);
        if((bits&128u)!=0u&&transport>1.0/255.0){facing=direction;attenuation=transport;}
    }
    if(dot(face,facing)>=0.0){return out;}
    var penalty=0.0;
    if(dot(hint,hint)>.25){let d=dot(face,hint);if(d>-.5&&sp.tierCounts.z<=.5){return out;}penalty=max(d+1.0,0.0);}
    let depth=textureLoad(cardDepth,pixel,0).r;if(!spFinite(depth)||depth<0.0){return out;}
    let delta=spCaptured(c,vec2<u32>(pixel),depth)-pos;let along=dot(delta,face);let perp=length(delta-face*along);
    let footprint=length(c.extent.xyz)/f32(max(res,1u));
    if(abs(along)>tolerance+footprint||perp>max(footprint*1.5,tolerance)){return out;}
    let value=textureLoad(cardLighting,pixel,0).rgb;if(!spFinite3(value)){return out;}
    out.rgb=value*attenuation;out.key=perp+.5*abs(along)+penalty*(perp+abs(along)+max(tolerance,.05));out.accepted=true;return out;
}
fn spCell(p:vec3<f32>)->u32 {
    if(arrayLength(&grid)<17u){return 0xffffffffu;}
    let origin=bitcast<vec3<f32>>(vec3<u32>(grid[0],grid[1],grid[2]));let cell=bitcast<f32>(grid[7]);
    let dims=vec3<u32>(grid[8],grid[9],grid[10]);let g=(p-origin)/cell;
    if(!spFinite3(g)||any(g<vec3<f32>(0))){return 0xffffffffu;}
    let c=vec3<u32>(g);if(any(c>=dims)){return 0xffffffffu;}return c.x+dims.x*(c.y+dims.y*c.z);
}
@compute @workgroup_size(64) fn measured_finalize_surface(@builtin(global_invocation_id) id:vec3<u32>) {
    let i=id.x;let count=u32(sp.grid.x)*u32(sp.grid.y)*u32(sp.grid.w);if(i>=count){return;}
    let probe=i/u32(sp.grid.w);var info=vec4<u32>(0,0xffffffffu,bitcast<u32>(1e30),0);
    if(positions[probe].w<=0.0){radiance[i]=vec4<f32>(0,0,0,-1);audit[i]=info;return;}
    let ray=rays[i];let hit=hits[i];let weight=max(ray.directionCosine.w,0.0)*ray.originWeight.w;
    var value=vec3<f32>(0);info.x=3u;
    if(hit.w==1.0){
        value=spBilinear(hit.yz);if(!spFinite3(value)){value=vec3<f32>(0);}
        if(sp.diagnostics.x<1.5||sp.diagnostics.x>2.5){value=spFirefly(value);}info.x=1u;
    }else if(hit.w==2.0&&sp.surfaceCache.z>.5&&all(textureDimensions(cardDepth)==textureDimensions(cardLighting))){
        let pos=ray.originWeight.xyz+ray.directionCosine.xyz*hit.x;let cell=spCell(pos);
        if(cell!=0xffffffffu&&cell<arrayLength(&grid)-17u){
            let begin=grid[16u+cell];let end=grid[17u+cell];
            var hint=vec3<f32>(0);if(sp.diagnostics.y>=0.0){hint=spHitNormal(hit.y)*(.5/max(sp.diagnostics.y,1e-3));}
            var best=1e30;
            if(begin<=end&&end<=arrayLength(&gridCards)){
                for(var at=begin;at<end;at++){
                    let ci=gridCards[at];if(ci>=arrayLength(&cards)||ci>=u32(sp.surfaceCache.y)){continue;}info.w++;
                    let candidate=spCandidate(cards[ci],pos,ray.directionCosine.xyz,hint);
                    if(candidate.accepted&&candidate.key<best){best=candidate.key;value=candidate.rgb;info.x=2u;info.y=ci;info.z=bitcast<u32>(best);}
                }
            }
        }
        if(sp.diagnostics.w<.5){value=spFirefly(value);}if(!spFinite3(value)){value=vec3<f32>(0);}
    }
    // Measured far-cache stops and rejected SDF cards both need qualified DDGI.
    if(info.x==3u){radiance[i]=vec4<f32>(0);}else{
        if(sp.diagnostics.x>3.5&&sp.diagnostics.x<4.5){value=vec3<f32>(0);}
        radiance[i]=vec4<f32>(value,weight);
    }
    audit[i]=info;
}
