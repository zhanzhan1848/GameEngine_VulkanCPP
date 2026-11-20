// Frozen tier3=0 placement: representative pixel, bounded jitter, F4/P1 gate.
@group(0) @binding(1) var placeDepth:texture_depth_2d;
@group(0) @binding(2) var placeNormal:texture_2d<f32>;
@group(0) @binding(3) var<storage,read_write> placePositions:array<vec4<f32>>;
@group(0) @binding(4) var<storage,read_write> placeNormals:array<vec4<f32>>;
fn validDepth(d:f32)->bool{return d>.0001&&d<.999;}
fn clampPixel(pixel:vec2<f32>)->vec2<i32>{return vec2<i32>(clamp(pixel,vec2<f32>(0),sp.trace.zw-1.0));}
fn placeWorld(pixel:vec2<i32>,depth:f32)->vec3<f32>{
    let uv=(vec2<f32>(pixel)+.5)/sp.trace.zw;let h=sp.inverseVP*vec4<f32>(2.0*uv.x-1.0,1.0-2.0*uv.y,depth,1);return h.xyz/h.w;
}
fn placeN(pixel:vec2<i32>)->vec3<f32>{return normalize(textureLoad(placeNormal,pixel,0).xyz*2.0-1.0);}
@compute @workgroup_size(64)
fn measured_place(@builtin(global_invocation_id) gid:vec3<u32>){
    let grid=vec2<u32>(sp.grid.xy);let probe=gid.x;if(probe>=grid.x*grid.y){return;}
    placePositions[probe]=vec4<f32>(0);placeNormals[probe]=vec4<f32>(0);
    let cell=vec2<u32>(probe%grid.x,probe/grid.x);let spacing=sp.grid.z;var center=vec2<f32>(cell)*spacing+spacing*.5;
    var representative=clampPixel(center);
    if(sp.representative.x>.5){
        let halfCell=spacing*.5;let offsets=array<vec2<f32>,5>(vec2<f32>(0),vec2<f32>(-halfCell,0),vec2<f32>(halfCell,0),vec2<f32>(0,-halfCell),vec2<f32>(0,halfCell));
        var depths:array<f32,5>;var pixels:array<vec2<i32>,5>;var sorted:array<f32,5>;var indices:array<u32,5>;var count=0u;
        for(var i=0u;i<5u;i++){
            pixels[i]=clampPixel(center+offsets[i]);depths[i]=textureLoad(placeDepth,pixels[i],0);
            if(!validDepth(depths[i])){continue;}var at=count;
            loop{if(at==0u){break;}if(!(sorted[at-1u]>depths[i])){break;}sorted[at]=sorted[at-1u];indices[at]=indices[at-1u];at--;}
            sorted[at]=depths[i];indices[at]=i;count++;
        }
        if(count==0u){return;}let median=sorted[count/2u];let medianN=placeN(pixels[indices[count/2u]]);var best=5u;var cost=3.4e38;
        for(var i=0u;i<5u;i++){if(!validDepth(depths[i])){continue;}
            let c=sp.representative.y*abs(depths[i]-median)+sp.representative.z*(1.0-max(dot(placeN(pixels[i]),medianN),0.0));
            if(c<cost){cost=c;best=i;}
        }
        if(best==5u){return;}representative=pixels[best];center=vec2<f32>(representative)+.5;
    }
    var jitter=vec2<f32>(0);if(sp.camera.w>=0.0&&sp.surfaceCache.w>0.0){jitter=spOffset(probe,sp.camera.w)*spacing*.25;}
    var pixel=clampPixel(center+jitter);var depth=textureLoad(placeDepth,pixel,0);
    if(any(jitter!=vec2<f32>(0))){let d=textureLoad(placeDepth,representative,0);let delta=distance(placeWorld(representative,d),placeWorld(pixel,depth));
        if(delta>sp.surfaceCache.w){pixel=clampPixel(center+jitter*(sp.surfaceCache.w/max(delta,1e-5)));depth=textureLoad(placeDepth,pixel,0);}}
    if(!validDepth(depth)){return;}
    if(((cell.x+cell.y)&1u)!=0u){
        var high=false;
        if(sp.cache.z>0.0){
            let p=placeWorld(pixel,depth);let neighbors=array<vec2<i32>,4>(vec2<i32>(cell)+vec2<i32>(-1,0),vec2<i32>(cell)+vec2<i32>(1,0),vec2<i32>(cell)+vec2<i32>(0,-1),vec2<i32>(cell)+vec2<i32>(0,1));
            var failed=false;var acc=vec3<f32>(0);var weight=0.0;
            for(var i=0u;i<4u;i++){let c=neighbors[i];if(any(c<vec2<i32>(0))||any(c>=vec2<i32>(grid))){continue;}
                let t=clampPixel(vec2<f32>(c)*spacing+f32(i32(spacing)/2));let d=textureLoad(placeDepth,t,0);if(!validDepth(d)){failed=true;break;}
                acc+=placeWorld(t,d)*.25;weight+=.25;
            }
            if(!failed&&weight>0.0){failed=distance(acc/weight,p)>sp.cache.z*(1.0+(1.0-weight));}
            if(!failed&&weight>0.0&&sp.representative.w>.5){let n=placeN(pixel);var minDot=1.0;
                for(var i=0u;i<4u;i++){let c=neighbors[i];if(any(c<vec2<i32>(0))||any(c>=vec2<i32>(grid))){continue;}
                    let t=clampPixel(vec2<f32>(c)*spacing+f32(i32(spacing)/2));minDot=min(minDot,dot(n,placeN(t)));}
                failed=minDot<sp.tier.y;
            }high=failed||weight<=0.0;
        }else{
            let n=placeN(pixel);var low=depth;var highDepth=depth;var minDot=1.0;let halfBlock=i32(spacing)/2;
            let offsets=array<vec2<i32>,4>(vec2<i32>(halfBlock,0),vec2<i32>(-halfBlock,0),vec2<i32>(0,halfBlock),vec2<i32>(0,-halfBlock));
            for(var i=0u;i<4u;i++){let t=clampPixel(vec2<f32>(pixel+offsets[i]));let d=textureLoad(placeDepth,t,0);low=min(low,d);highDepth=max(highDepth,d);minDot=min(minDot,dot(n,placeN(t)));}
            high=highDepth-low>.0015||minDot<.8;
        }
        if(!high){return;}
    }
    placePositions[probe]=vec4<f32>(placeWorld(pixel,depth),1);placeNormals[probe]=vec4<f32>(placeN(pixel),1);
}
