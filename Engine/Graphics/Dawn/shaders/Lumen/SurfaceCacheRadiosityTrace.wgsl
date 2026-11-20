@group(0) @binding(0) var<uniform> p:Params;
@group(0) @binding(1) var<storage,read> cards:array<Card>;
@group(0) @binding(2) var<storage,read> owners:array<u32>;
@group(0) @binding(3) var<storage,read_write> hits:array<Hit>;
@group(0) @binding(4) var depthAtlas:texture_2d<f32>;
@group(0) @binding(5) var normalAtlas:texture_2d<f32>;
@group(0) @binding(6) var sdf:texture_3d<f32>;
fn sampleSdf(pos:vec3<f32>)->f32{let uv=(pos-p.origin.xyz)/p.extent.xyz;if(any(uv<vec3<f32>(0))||any(uv>vec3<f32>(1))){return 1e6;}let size=vec3<i32>(textureDimensions(sdf));return textureLoad(sdf,clamp(vec3<i32>(clamp(uv,vec3<f32>(0),vec3<f32>(1))*vec3<f32>(size)),vec3<i32>(0),size-1),0).r;}
@compute @workgroup_size(8,8) fn tile_trace(@builtin(global_invocation_id) id:vec3<u32>){
 let width=p.counts.x/p.counts.y;if(any(id.xy>=vec2<u32>(width))){return;}let tile=id.y*width+id.x;if(p.schedule.y>1u&&tile%p.schedule.y!=p.schedule.x%p.schedule.y){return;}
 for(var r=0u;r<p.counts.z;r++){hits[tile*p.counts.z+r]=Hit(vec4<f32>(0),vec4<u32>(4,0,0,0));}
 let pixel=id.xy*p.counts.y+p.counts.y/2u;let depth=textureLoad(depthAtlas,vec2<i32>(pixel),0).r;let owner=owners[tile];if(!finite(depth)||depth<=0||owner>=p.counts.w){return;}
 let n=octDecode(textureLoad(normalAtlas,vec2<i32>(pixel),0).rg);let pos=world(pixel,depth,cards[owner]);if(!finite3(n)||!finite3(pos)){return;}
 let voxel=p.extent.x/max(p.origin.w,1);let band=max(voxel*1.25,.30);let minStep=voxel*.25;let origin=pos+n*(band+.05);
 for(var r=0u;r<p.counts.z;r++){
  let dir=direction(tile,r,p.schedule.x,n);var t=0.0;var found=false;var cleared=false;var point=vec3<f32>(0);
  for(var step=0u;step<32u;step++){let at=origin+dir*t;let d=sampleSdf(at);if(d<band){if(!cleared){t+=max(abs(d),max(minStep,.05));if(t>p.policy.y){break;}continue;}found=true;point=at;break;}cleared=true;if(t>p.policy.y){break;}t+=select(max(d,minStep),d*1.2,d>band*4.0);}
  // The frozen offline profile binds the base SDF as every mip; cone check
  // is therefore disabled. Lack of origin clearance remains occluded type 3.
  var kind=select(3u,2u,cleared);if(found){kind=select(1u,0u,t<p.policy.x);}
  hits[tile*p.counts.z+r]=Hit(vec4<f32>(point,t),vec4<u32>(kind,bitcast<vec3<u32>>(origin)));
 }
}
