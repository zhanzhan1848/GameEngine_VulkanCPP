@group(0) @binding(0) var<uniform> p:Params;
@group(0) @binding(1) var<storage,read> lookups:array<Lookup>;
@group(0) @binding(2) var<storage,read> cards:array<Card>;
@group(0) @binding(3) var<storage,read> hits:array<Hit>;
@group(0) @binding(4) var<storage,read_write> valid:array<atomic<u32>>;
@group(0) @binding(5) var depthAtlas:texture_2d<f32>;
@group(0) @binding(6) var normalAtlas:texture_2d<f32>;
@group(0) @binding(7) var previousLighting:texture_2d<f32>;
@group(0) @binding(8) var previousGrid:texture_2d<f32>;
@group(0) @binding(9) var gridOut:texture_storage_2d<rgba16float,write>;
@group(0) @binding(10) var auxOut:texture_storage_2d<rgba16float,write>;
struct Candidate {value:vec3<f32>,key:f32,valid:bool}
fn candidate(c:Card,pos:vec3<f32>,facing:vec3<f32>,band:f32)->Candidate{
 var out:Candidate;out.valid=false;out.key=1e30;out.value=vec3<f32>(0);let axis=c.placement.x&255u;let local=pos-c.center.xyz;let normalFace=face(c);if(dot(local,normalFace)<0||dot(normalFace,facing)>=0){return out;}
 var uv=local.xy/(c.extent.xy*2)+.5;if(axis==0u){uv=local.yz/(c.extent.yz*2)+.5;}else if(axis==1u){uv=local.xz/(c.extent.xz*2)+.5;}
 let margin=1.0/f32(max(c.placement.y,1u));if(any(uv<vec2<f32>(-margin))||any(uv>=vec2<f32>(1+margin))){return out;}
 let pixel=vec2<u32>(clamp(vec2<f32>(c.placement.zw)+clamp(uv,vec2<f32>(0),vec2<f32>(.999999))*f32(c.placement.y),vec2<f32>(0),vec2<f32>(c.placement.zw+c.placement.y-1u)));
 let d=textureLoad(depthAtlas,vec2<i32>(pixel),0).r;if(!finite(d)||d<0){return out;}let delta=world(pixel,d,c)-pos;let along=dot(delta,normalFace);let lateral=length(delta-normalFace*along);let footprint=length(c.extent.xyz)/f32(max(c.placement.y,1u));if(abs(along)>band+footprint||lateral>max(footprint*1.5,band)){return out;}
 let rgb=textureLoad(previousLighting,vec2<i32>(pixel),0).rgb;if(!finite3(rgb)){return out;}out.value=rgb;out.key=lateral+.5*abs(along);out.valid=true;return out;
}
@compute @workgroup_size(8,8) fn tile_resolve(@builtin(global_invocation_id) id:vec3<u32>){
 let width=p.counts.x/p.counts.y;if(any(id.xy>=vec2<u32>(width))){return;}let tile=id.y*width+id.x;let refresh=p.schedule.y<=1u||tile%p.schedule.y==p.schedule.x%p.schedule.y;
 let mask=1u<<(tile&31u);let wasValid=(atomicOr(&valid[tile>>5u],0u)&mask)!=0u;let hasHistory=p.schedule.x>=2u;let at=vec2<i32>(id.xy);let pixel=id.xy*p.counts.y+p.counts.y/2u;
 let enc=textureLoad(normalAtlas,vec2<i32>(pixel),0).rg;let n=octDecode(enc);var sum=vec3<f32>(0);var current=0u;var measured=0u;let band=max(p.extent.x/max(p.origin.w,1)*1.25,.30);
 if(refresh){for(var r=0u;r<p.counts.z;r++){
  let h=hits[tile*p.counts.z+r];let kind=h.identity.x&7u;if(kind>=4u){continue;}current++;if(kind==3u){measured++;continue;}let dir=direction(tile,r,p.schedule.x,n);var rgb=vec3<f32>(0);
  if(kind<=1u){var found=false;var best=1e30;
   // Preserve frozen bucket order: minimum key in the FIRST matching
   // lookup with an accepted candidate, not a global-nearest substitution.
   for(var li=0u;li<p.schedule.z&&!found;li++){let l=lookups[li];if(any(h.position.xyz<l.minimum.xyz)||any(h.position.xyz>l.maximum.xyz)){continue;}
    for(var ci=l.range.x;ci<l.range.x+l.range.y;ci++){let c=candidate(cards[ci],h.position.xyz,-dir,max(band,.15));if(c.valid&&c.key<best){best=c.key;rgb=c.value;found=true;}}
   }if(!found){continue;}
  }else{let weight=pow(clamp(dir.y,0.0,1.0),.45);rgb=mix(vec3<f32>(4.4,4.2,3.8),vec3<f32>(5.5,5.76,6.0),weight)*p.extent.w;}
  sum+=rgb;measured++;
 }}
 let usable=refresh&&current==p.counts.z&&measured>=min(p.counts.z,4u);if(usable){sum/=f32(max(p.counts.z,1u));atomicOr(&valid[tile>>5u],mask);}else{sum=vec3<f32>(0);}
 let previous=textureLoad(previousGrid,at,0).rgb;if(p.policy.w>0&&usable&&wasValid&&hasHistory&&all(previous>=vec3<f32>(0))){sum=min(sum,previous+previous*p.policy.w+.02);}
 var value=sum;var alpha=1.0;let tw=clamp(p.policy.z,0.0,.95);
 if(!usable){alpha=select(0.0,1.0,wasValid&&hasHistory);value=select(vec3<f32>(0),previous,wasValid&&hasHistory);}else if(hasHistory&&tw>0){value=mix(sum,previous,tw);}
 textureStore(gridOut,at,vec4<f32>(value,alpha));var owner=4095u;
 for(var ci=0u;ci<p.counts.w;ci++){let c=cards[ci];if(c.placement.y!=0u&&all(pixel>=c.placement.zw)&&all(pixel<c.placement.zw+c.placement.y)){owner=ci;break;}}
 textureStore(auxOut,at,vec4<f32>(enc,textureLoad(depthAtlas,vec2<i32>(pixel),0).r,f32(owner)));
}
