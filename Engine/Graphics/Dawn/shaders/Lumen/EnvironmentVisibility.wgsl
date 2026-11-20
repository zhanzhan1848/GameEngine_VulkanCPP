// Native WGSL environment-lobe visibility. Frozen scalar Vulkan contract.
// All-escape/known endpoints are categorical; UNKNOWN never opens geometry.
struct GeometryNode {lo:vec4<f32>,hi:vec4<f32>,links:vec4<u32>}
struct GeometryTriangle {a:vec4<f32>,e1:vec4<f32>,e2:vec4<f32>}
struct GeometryInstance {inverse0:vec4<f32>,inverse1:vec4<f32>,inverse2:vec4<f32>,identity:vec4<u32>}
struct VisibilityParams {inverseVP:mat4x4<f32>,camera:vec4<f32>,dimensions:vec4<u32>,policy:vec4<u32>,parameters:vec4<f32>,outputGrid:vec4<u32>,previousVP:mat4x4<f32>}
@group(0) @binding(0) var depthTexture:texture_depth_2d;
@group(0) @binding(1) var normalTexture:texture_2d<f32>;
@group(0) @binding(2) var ormTexture:texture_2d<f32>;
@group(0) @binding(3) var outputVisibility:texture_storage_2d<rgba16float,write>;
@group(0) @binding(4) var<uniform> cb:VisibilityParams;
@group(0) @binding(5) var halfVisibility:texture_2d<f32>;
@group(0) @binding(13) var<storage,read> geometryTop:array<GeometryNode>;
@group(0) @binding(14) var<storage,read> geometryBottom:array<GeometryNode>;
@group(0) @binding(15) var<storage,read> geometryTriangles:array<GeometryTriangle>;
@group(0) @binding(16) var<storage,read> geometryInstances:array<GeometryInstance>;
fn finite1(v:f32)->bool{return (bitcast<u32>(v)&0x7f800000u)!=0x7f800000u;}
fn finite3(v:vec3<f32>)->bool{return finite1(v.x)&&finite1(v.y)&&finite1(v.z);}
fn finite4(v:vec4<f32>)->bool{return finite3(v.xyz)&&finite1(v.w);}
fn differenceOfProducts(a:f32,b:f32,c:f32,d:f32)->f32{let cd=c*d;return fma(a,b,-cd)+fma(-c,d,cd);}
fn triangleNormal(a:vec3<f32>,b:vec3<f32>)->vec3<f32>{return vec3<f32>(differenceOfProducts(a.y,b.z,a.z,b.y),differenceOfProducts(a.z,b.x,a.x,b.z),differenceOfProducts(a.x,b.y,a.y,b.x));}
fn nodeValid(n:GeometryNode)->bool{return finite3(n.lo.xyz)&&finite3(n.hi.xyz)&&all(n.lo.xyz<=n.hi.xyz);}
fn box(n:GeometryNode,o:vec3<f32>,d:vec3<f32>,nearT:f32,farT:f32)->bool{
 var lo=nearT;var hi=farT;
 for(var k=0u;k<3u;k++){if(d[k]==0.){if(o[k]<n.lo[k]||o[k]>n.hi[k]){return false;}}else{let a=(n.lo[k]-o[k])/d[k];let b=(n.hi[k]-o[k])/d[k];lo=max(lo,min(a,b));hi=min(hi,max(a,b));if(lo>hi){return false;}}}return true;
}
fn hash32(value:u32)->u32{var x=value;x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;return x^(x>>16u);}
fn random2(seed:u32)->vec2<f32>{return (vec2<f32>(f32(hash32(seed^0x68bc21ebu)>>8u),f32(hash32(seed^0x02e5be93u)>>8u))+.5)/16777216.;}
fn traceToSource(pixel:vec2<i32>)->vec2<i32>{let size=vec2<i32>(cb.dimensions.xy);let trace=vec2<i32>(cb.dimensions.zw);let ratio=max(vec2<i32>(1),(size+trace-1)/trace);return min(pixel*ratio+ratio/2,size-1);}
fn pointAt(pixel:vec2<i32>)->vec4<f32>{
 let size=vec2<i32>(cb.dimensions.xy);if(any(pixel<vec2<i32>(0))||any(pixel>=size)){return vec4<f32>(0);}
 let z=textureLoad(depthTexture,pixel,0);if(!finite1(z)||z<0.||z>=.999999){return vec4<f32>(0);}
 let uv=(vec2<f32>(pixel)+.5)/vec2<f32>(size);let ndc=vec2<f32>(uv.x*2.-1.,1.-uv.y*2.)-vec2<f32>(cb.camera.w,bitcast<f32>(cb.policy.z));
 if(!finite1(ndc.x)||!finite1(ndc.y)){return vec4<f32>(0);}let h=cb.inverseVP*vec4<f32>(ndc,z,1);
 if(!finite1(h.w)||abs(h.w)<1e-8){return vec4<f32>(0);}let p=h.xyz/h.w;if(!finite3(p)){return vec4<f32>(0);}return vec4<f32>(p,1);
}
struct Receiver {p:vec3<f32>,ng:vec3<f32>,ns:vec3<f32>,v:vec3<f32>,roughness:f32,valid:bool}
fn receiver(pixel:vec2<i32>)->Receiver{
 var r:Receiver;if((cb.policy.y&0x80000000u)!=0u){return r;}
 if(any(textureDimensions(depthTexture)!=cb.dimensions.xy)||any(textureDimensions(normalTexture)!=cb.dimensions.xy)||any(textureDimensions(ormTexture)!=cb.dimensions.xy)){return r;}
 let p=pointAt(pixel);if(p.w==0.){return r;}r.p=p.xyz;let view=cb.camera.xyz-r.p;if(!finite3(view)||dot(view,view)<1e-10){return r;}r.v=normalize(view);
 let a=pointAt(pixel-vec2<i32>(1,0));let b=pointAt(pixel+vec2<i32>(1,0));let c=pointAt(pixel-vec2<i32>(0,1));let d=pointAt(pixel+vec2<i32>(0,1));
 if((a.w==0.&&b.w==0.)||(c.w==0.&&d.w==0.)){return r;}
 let dx=select(b.xyz-r.p,r.p-a.xyz,a.w!=0.&&(b.w==0.||distance(a.xyz,r.p)<distance(b.xyz,r.p)));
 let dy=select(d.xyz-r.p,r.p-c.xyz,c.w!=0.&&(d.w==0.||distance(c.xyz,r.p)<distance(d.xyz,r.p)));
 let limit=max(.02,length(view)*.02);if(length(dx)>limit||length(dy)>limit){return r;}
 let n=cross(dx,dy);let scale=max(abs(n.x),max(abs(n.y),abs(n.z)));if(!finite3(n)||scale<1e-16){return r;}r.ng=normalize(n/scale);if(dot(r.ng,r.v)<0.){r.ng=-r.ng;}
 let ns=textureLoad(normalTexture,pixel,0).xyz*2.-1.;if(!finite3(ns)||dot(ns,ns)<.25){return r;}r.ns=normalize(ns);if(dot(r.ns,r.v)<=1e-5||dot(r.ns,r.ng)<=0.){return r;}
 let rough=textureLoad(ormTexture,pixel,0).g;if(!finite1(rough)||rough<0.||rough>1.){return r;}r.roughness=max(rough,.04);r.valid=true;return r;
}
// Existential opaque witness. A complete traversal without a witness is needed
// for MISS; malformed nodes or boundary-only intersections remain UNKNOWN.
fn environmentAnyHit(origin:vec3<f32>,direction:vec3<f32>,nearT:f32,farT:f32)->i32{
 if(!finite3(origin)||!finite3(direction)||!finite1(nearT)||!finite1(farT)||nearT<0.||farT<=nearT||dot(direction,direction)<=0.){return -1;}
 var endpoint=false;var topStack:array<u32,64>;var topSize=1u;var topVisited=0u;topStack[0]=0u;
 loop{if(topSize==0u){break;}if(topVisited>=arrayLength(&geometryTop)){return -1;}topVisited++;topSize--;var at=topStack[topSize];if(at>=arrayLength(&geometryTop)){return -1;}
  let node=geometryTop[at];if(!nodeValid(node)){return -1;}if(!box(node,origin,direction,nearT,farT)){continue;}
  if(node.links.w==0u){if(topSize+2u>64u||node.links.x>=arrayLength(&geometryTop)||node.links.y>=arrayLength(&geometryTop)){return -1;}topStack[topSize]=node.links.x;topStack[topSize+1u]=node.links.y;topSize+=2u;continue;}
  if(node.links.w!=1u||node.links.z>=arrayLength(&geometryInstances)){return -1;}let instance=geometryInstances[node.links.z];
  if(!finite4(instance.inverse0)||!finite4(instance.inverse1)||!finite4(instance.inverse2)){return -1;}
  let invDet=dot(instance.inverse0.xyz,cross(instance.inverse1.xyz,instance.inverse2.xyz));if(!finite1(invDet)||invDet==0.){return -1;}
  let o=vec3<f32>(dot(instance.inverse0,vec4<f32>(origin,1)),dot(instance.inverse1,vec4<f32>(origin,1)),dot(instance.inverse2,vec4<f32>(origin,1)));
  let d=vec3<f32>(dot(instance.inverse0.xyz,direction),dot(instance.inverse1.xyz,direction),dot(instance.inverse2.xyz,direction));let ds=dot(d,d);if(!finite3(o)||!finite3(d)||!finite1(ds)||ds<=0.){return -1;}
  var stack:array<u32,64>;var size=1u;var visited=0u;stack[0]=instance.identity.x;
  loop{if(size==0u){break;}if(visited>=arrayLength(&geometryBottom)){return -1;}visited++;size--;at=stack[size];if(at>=arrayLength(&geometryBottom)){return -1;}
   let bottom=geometryBottom[at];if(!nodeValid(bottom)){return -1;}if(!box(bottom,o,d,nearT,farT)){continue;}
   if(bottom.links.w==0u){if(size+2u>64u||bottom.links.x>=arrayLength(&geometryBottom)||bottom.links.y>=arrayLength(&geometryBottom)){return -1;}stack[size]=bottom.links.x;stack[size+1u]=bottom.links.y;size+=2u;continue;}
   if(bottom.links.z>arrayLength(&geometryTriangles)||bottom.links.w>arrayLength(&geometryTriangles)-bottom.links.z){return -1;}
   for(var i=0u;i<bottom.links.w;i++){
    let tri=geometryTriangles[bottom.links.z+i];if(!finite3(tri.a.xyz)||!finite3(tri.e1.xyz)||!finite3(tri.e2.xyz)){return -1;}
    let p=cross(d,tri.e2.xyz);let det=dot(tri.e1.xyz,p);if(!finite1(det)){return -1;}if(det==0.){continue;}let delta=o-tri.a.xyz;let u=dot(delta,p)/det;if(!finite1(u)){return -1;}if(u<0.||u>1.){continue;}
    let q=cross(delta,tri.e1.xyz);let v=dot(d,q)/det;if(!finite1(v)){return -1;}if(v<0.||u+v>1.){continue;}
    let n=triangleNormal(tri.e1.xyz,tri.e2.xyz);let planeDet=dot(d,n);if(!finite3(n)||!finite1(planeDet)||planeDet==0.){return -1;}let t=dot(tri.a.xyz-o,n)/planeDet;if(!finite1(t)){return -1;}if(t<nearT||t>farT){continue;}
    if(t<=nearT+2e-5||t>=farT-2e-5){endpoint=true;continue;}return 1;
   }
  }
 }return select(0,-1,endpoint);
}
fn environmentRay(r:Receiver,direction:vec3<f32>)->i32{
 if(!finite3(direction)){return 0;}if(dot(direction,r.ng)<=0.){return 1;}
 if(cb.policy.x!=1u||arrayLength(&geometryTop)==0u||arrayLength(&geometryBottom)==0u||arrayLength(&geometryTriangles)==0u||arrayLength(&geometryInstances)==0u){return 0;}
 let root=geometryTop[0];if(!nodeValid(root)){return 0;}let origin=r.p+r.ng*cb.parameters.x;var enter=0.;var leave=3.402823466e38f;
 for(var k=0u;k<3u;k++){if(direction[k]==0.){if(origin[k]<root.lo[k]||origin[k]>root.hi[k]){return 2;}}else{let a=(root.lo[k]-origin[k])/direction[k];let b=(root.hi[k]-origin[k])/direction[k];enter=max(enter,min(a,b));leave=min(leave,max(a,b));if(enter>leave){return 2;}}}
 if(!finite1(leave)){return 0;}if(leave<0.){return 2;}let farT=leave+max(.001,abs(leave)*1e-5);let hit=environmentAnyHit(origin,direction,0.,farT);if(hit<0){return 0;}return select(2,1,hit>0);
}
fn sampleVisibleGGX(view:vec3<f32>,alpha:f32,xi:vec2<f32>)->vec3<f32>{
 let vh=normalize(vec3<f32>(alpha*view.xy,view.z));var t1=vec3<f32>(1,0,0);if(vh.z<.99999){t1=normalize(cross(vec3<f32>(0,0,1),vh));}let t2=cross(vh,t1);let radius=sqrt(xi.x);let phi=6.28318530718*xi.y;let a=radius*cos(phi);var b=radius*sin(phi);let s=.5*(1.+vh.z);b=(1.-s)*sqrt(max(0.,1.-a*a))+s*b;let nh=a*t1+b*t2+sqrt(max(0.,1.-a*a-b*b))*vh;return normalize(vec3<f32>(alpha*nh.xy,max(0.,nh.z)));
}
fn traceLobe(r:Receiver,pixel:vec2<i32>,firstRay:u32,rayCount:u32)->vec4<f32>{
 if(cb.policy.y==1u){return vec4<f32>(1,0,1,1);}if(cb.policy.x!=1u){return vec4<f32>(0,0,1,1);}
 var axis=vec3<f32>(1,0,0);if(abs(r.ns.z)<.999){axis=vec3<f32>(0,0,1);}let t=normalize(cross(axis,r.ns));let b=cross(r.ns,t);let v=vec3<f32>(dot(r.v,t),dot(r.v,b),dot(r.v,r.ns));
 let count=select(4u,1u,r.roughness<=.06);var visible=0.;var known=0.;var total=0.;var anyUnknown=false;var anyEscape=false;var allEscape=true;let alpha=r.roughness*r.roughness;let seed=(u32(pixel.x)*0x9e3779b9u)^(u32(pixel.y)*0x85ebca6bu);let rotation=random2(seed);
 for(var i=firstRay;i<count&&i<firstRay+rayCount;i++){
  var localL=vec3<f32>(-v.xy,v.z);var weight=1.;if(count!=1u){let xi=fract(vec2<f32>((f32(i)+.5)/4.,f32((i&1u)*2u+(i>>1u))*.25+.125)+rotation);let h=sampleVisibleGGX(v,alpha,xi);localL=reflect(-v,h);if(localL.z<=0.){continue;}let c=localL.z;weight=2.*c/(c+sqrt(alpha*alpha+(1.-alpha*alpha)*c*c));}
  let worldL=normalize(t*localL.x+b*localL.y+r.ns*localL.z);let outcome=environmentRay(r,worldL);total+=weight;anyUnknown=anyUnknown||outcome==0;anyEscape=anyEscape||outcome==2;allEscape=allEscape&&outcome==2;if(outcome!=0){known+=weight;}if(outcome==2){visible+=weight;}
 }
 if(total<=0.){return vec4<f32>(0,1,0,1);}var visibility=0.;if(allEscape){visibility=1.;}else if(anyEscape){visibility=clamp(visible/total,0.,1.);}var fraction=1.;if(anyUnknown){fraction=clamp(known/total,0.,1.);}return vec4<f32>(visibility,fraction,select(0.,1.,anyUnknown),1);
}
fn mirrorFallback(r:Receiver)->vec4<f32>{if(cb.policy.y==1u){return vec4<f32>(1,0,2,1);}if(cb.policy.x!=1u){return vec4<f32>(0,0,3,1);}let result=environmentRay(r,normalize(reflect(-r.v,r.ns)));return vec4<f32>(select(0.,1.,result==2),select(0.,1.,result!=0),select(3.,2.,result!=0),1);}
@compute @workgroup_size(8,8)
fn environment_visibility_trace(@builtin(global_invocation_id) gid:vec3<u32>){
 if(any(gid.xy>=cb.dimensions.zw)){return;}let pixel=vec2<i32>(gid.xy);let source=traceToSource(pixel);let r=receiver(source);if(!r.valid){textureStore(outputVisibility,pixel,vec4<f32>(0,0,1,0));return;}
 if((cb.outputGrid.z&1u)==0u){textureStore(outputVisibility,pixel,traceLobe(r,source,0u,4u));return;}let current=traceLobe(r,source,cb.policy.w&3u,1u);var result=current;
 if(cb.policy.w>=4u){let clip=cb.previousVP*vec4<f32>(r.p,1);if(finite1(clip.w)&&clip.w>1e-6){let ndc=clip.xy/clip.w;let uv=vec2<f32>(ndc.x*.5+.5,.5-ndc.y*.5);let hp=vec2<i32>(uv*vec2<f32>(cb.dimensions.zw));if(all(hp>=vec2<i32>(0))&&all(hp<vec2<i32>(cb.dimensions.zw))){let history=textureLoad(halfVisibility,hp,0);let hr=receiver(traceToSource(hp));if(history.a>=.5&&finite4(history)&&hr.valid&&distance(hr.p,r.p)<=max(cb.parameters.x,length(cb.camera.xyz-r.p)*.02)){result=mix(history,current,.25);}}}}
 textureStore(outputVisibility,pixel,result);
}
@compute @workgroup_size(8,8)
fn environment_visibility_resolve(@builtin(global_invocation_id) gid:vec3<u32>){
 if(any(gid.xy>=cb.outputGrid.xy)){return;}let pixel=vec2<i32>(gid.xy);let sourcePixel=clamp(vec2<i32>((vec2<f32>(pixel)+.5)/vec2<f32>(cb.outputGrid.xy)*vec2<f32>(cb.dimensions.xy)),vec2<i32>(0),vec2<i32>(cb.dimensions.xy)-1);let dest=receiver(sourcePixel);if(!dest.valid){textureStore(outputVisibility,pixel,vec4<f32>(0,0,1,0));return;}
 var best=3.402823466e38f;var result=vec4<f32>(0);var found=false;let blend=(cb.outputGrid.w&1u)!=0u;var accum=vec4<f32>(0);var weight=0.;let base=max((pixel-1)/2,vec2<i32>(0));
 for(var y=0;y<=1;y++){for(var x=0;x<=1;x++){let h=base+vec2<i32>(x,y);if(any(h<vec2<i32>(0))||any(h>=vec2<i32>(cb.dimensions.zw))){continue;}let src=receiver(traceToSource(h));if(!src.valid){continue;}let delta=src.p-dest.p;let nsGate=mix(.9995,.9,dest.roughness*dest.roughness);
 if(dot(src.ng,dest.ng)<cb.parameters.z||dot(src.ns,dest.ns)<nsGate||abs(src.roughness-dest.roughness)>cb.parameters.w||abs(dot(delta,dest.ng))>cb.parameters.y||abs(dot(delta,src.ng))>cb.parameters.y){continue;}let key=dot(delta,delta);let view=cb.camera.xyz-dest.p;if(key>max(.0025,dot(view,view)*.000025)){continue;}let value=textureLoad(halfVisibility,h,0);if(!finite4(value)||value.a<.5){continue;}if(key<best){best=key;result=value;found=true;}if(blend){let w=1./(1e-3+key);accum+=value*w;weight+=w;}
 }}if(blend&&found&&weight>0.){result=accum/weight;}if(!found){result=mirrorFallback(dest);}textureStore(outputVisibility,pixel,result);
}
