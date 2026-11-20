// Native WGSL for the frozen default offline-SDF tile radiosity profile.
struct Params {counts:vec4<u32>,schedule:vec4<u32>,policy:vec4<f32>,origin:vec4<f32>,extent:vec4<f32>}
struct Card {center:vec4<f32>,extent:vec4<f32>,placement:vec4<u32>,owner:vec4<u32>}
struct Lookup {minimum:vec4<f32>,maximum:vec4<f32>,range:vec4<u32>}
struct Hit {position:vec4<f32>,identity:vec4<u32>}
fn finite(x:f32)->bool{return (bitcast<u32>(x)&0x7f800000u)!=0x7f800000u;}
fn finite3(x:vec3<f32>)->bool{return finite(x.x)&&finite(x.y)&&finite(x.z);}
fn octDecode(enc:vec2<f32>)->vec3<f32>{let f=enc*2.0-1.0;var n=vec3<f32>(f,1.0-abs(f.x)-abs(f.y));if(n.z<0){n=vec3<f32>((1.0-abs(n.yx))*select(vec2<f32>(-1),vec2<f32>(1),n.xy>=vec2<f32>(0)),n.z);}return normalize(n);}
fn face(c:Card)->vec3<f32>{let s=select(-1.0,1.0,((c.placement.x>>8u)&255u)>0u);let a=c.placement.x&255u;if(a==0u){return vec3<f32>(s,0,0);}if(a==1u){return vec3<f32>(0,s,0);}return vec3<f32>(0,0,s);}
fn world(pixel:vec2<u32>,depth:f32,c:Card)->vec3<f32>{let uv=(vec2<f32>(pixel-c.placement.zw)+.5)/f32(c.placement.y)-.5;let a=c.placement.x&255u;let s=select(-1.0,1.0,((c.placement.x>>8u)&255u)>0u);if(a==0u){return c.center.xyz+vec3<f32>(s*depth,uv.x*c.extent.y*2,uv.y*c.extent.z*2);}if(a==1u){return c.center.xyz+vec3<f32>(uv.x*c.extent.x*2,s*depth,uv.y*c.extent.z*2);}return c.center.xyz+vec3<f32>(uv.x*c.extent.x*2,uv.y*c.extent.y*2,s*depth);}
fn hash32(value:u32)->u32{var x=value;x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;x^=x>>16u;return x;}
fn direction(tile:u32,ray:u32,frame:u32,n:vec3<f32>)->vec3<f32>{let seed=(tile*0x9e3779b9u)^(ray*0x85ebca6bu)^(frame*0xc2b2ae35u);let xi=vec2<f32>(f32(hash32(seed^0x68bc21ebu)>>8u),f32(hash32(seed^0x02e5be93u)>>8u))*(1.0/16777216.0);let angle=xi.y*2.0*3.14159265;let a=sqrt(1.0-xi.x);let local=vec3<f32>(a*cos(angle),a*sin(angle),sqrt(xi.x));let up=select(vec3<f32>(1,0,0),vec3<f32>(0,1,0),abs(n.y)<.999);let t=normalize(cross(up,n));return normalize(t*local.x+cross(n,t)*local.y+n*local.z);}
