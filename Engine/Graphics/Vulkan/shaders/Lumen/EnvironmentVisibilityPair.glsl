// Private, not selected by the frozen Environment Trace. Call only after each
// original environmentRay precondition has produced a valid origin/farT.
// False means physical shared stack overflow: caller must discard BOTH results
// and re-run both original environmentRay calls before output accumulation.
#ifndef ENV_PAIR_PHYSICAL_LIMIT
#define ENV_PAIR_PHYSICAL_LIMIT 64
#endif
#if ENV_PAIR_PHYSICAL_LIMIT > 64 || ENV_PAIR_PHYSICAL_LIMIT < 1
#error Environment pair physical limit outside original 64-entry stack
#endif
struct EnvironmentPairResult {ivec2 status;
#ifdef ENV_PAIR_DIAGNOSTICS
 uvec4 counts0,counts1;uvec2 order;uint nodeLoads,instanceLoads,triangleLoads;
#endif
};
void envPairMaskSet(inout uvec4 bits,uint at,uint m){uint i=at>>4u,shift=(at&15u)*2u;bits[i]=(bits[i]&~(3u<<shift))|((m&3u)<<shift);}
uint envPairMaskGet(uvec4 bits,uint at){return (bits[at>>4u]>>((at&15u)*2u))&3u;}
#ifdef ENV_PAIR_DIAGNOSTICS
uint envPairDigest(uint h,uint kind,uint id,uint inst){h=(h^kind)*16777619u;h=(h^id)*16777619u;return (h^inst)*16777619u;}
#endif
bool environmentAnyHitPair(vec3 origin[2],vec3 direction[2],float farT[2],out EnvironmentPairResult outputPair){
 outputPair.status=ivec2(0);bvec2 endpoint=bvec2(false);
#ifdef ENV_PAIR_DIAGNOSTICS
 outputPair.counts0=uvec4(0);outputPair.counts1=uvec4(0);outputPair.order=uvec2(2166136261u);outputPair.nodeLoads=0u;outputPair.instanceLoads=0u;outputPair.triangleLoads=0u;
#endif
 uint stack[64];uvec4 maskBits=uvec4(0);uint sp=1u;stack[0]=0u;envPairMaskSet(maskBits,0u,3u);uint live=3u;uvec2 pending=uvec2(1u),visited=uvec2(0u);
 while(sp!=0u && live!=0u){--sp;uint at=stack[sp],m=envPairMaskGet(maskBits,sp)&live;if(m==0u)continue;
  for(uint r=0u;r<2u;++r)if((m&(1u<<r))!=0u){if(visited[r]++>=geometryTop.data.length()){outputPair.status[r]=-1;live&=~(1u<<r);m&=~(1u<<r);}else{--pending[r];
#ifdef ENV_PAIR_DIAGNOSTICS
   if(r==0u)++outputPair.counts0.x;else ++outputPair.counts1.x;outputPair.order[r]=envPairDigest(outputPair.order[r],1u,at,0u);
#endif
  }}
  if(m==0u)continue;if(at>=geometryTop.data.length()){for(uint r=0u;r<2u;++r)if((m&(1u<<r))!=0u){outputPair.status[r]=-1;live&=~(1u<<r);}continue;}
  GeometryNode node=geometryTop.data[at];
#ifdef ENV_PAIR_DIAGNOSTICS
  ++outputPair.nodeLoads;
#endif
  for(uint r=0u;r<2u;++r)if((m&(1u<<r))!=0u && !environmentNodeValid(node)){outputPair.status[r]=-1;live&=~(1u<<r);m&=~(1u<<r);}
  if(m==0u)continue;
  uint hits=0u;for(uint r=0u;r<2u;++r)if((m&(1u<<r))!=0u && geometryBox(node,origin[r],direction[r],0.,farT[r]))hits|=1u<<r;if(hits==0u)continue;
  if(node.links.w==0u){uint push=hits;for(uint r=0u;r<2u;++r)if((hits&(1u<<r))!=0u){if(pending[r]+2u>64u||node.links.x>=geometryTop.data.length()||node.links.y>=geometryTop.data.length()){outputPair.status[r]=-1;live&=~(1u<<r);push&=~(1u<<r);}else pending[r]+=2u;}
   if(push!=0u){if(sp+2u>uint(ENV_PAIR_PHYSICAL_LIMIT))return false;stack[sp]=node.links.x;envPairMaskSet(maskBits,sp++,push);stack[sp]=node.links.y;envPairMaskSet(maskBits,sp++,push);}continue;}
  if(node.links.w!=1u||node.links.z>=geometryInstances.data.length()){for(uint r=0u;r<2u;++r)if((hits&(1u<<r))!=0u){outputPair.status[r]=-1;live&=~(1u<<r);}continue;}
  GeometryInstance inst=geometryInstances.data[node.links.z];
#ifdef ENV_PAIR_DIAGNOSTICS
  ++outputPair.instanceLoads;
#endif
  vec3 localOrigin[2],localDirection[2];uint bm=hits;
  for(uint r=0u;r<2u;++r)if((hits&(1u<<r))!=0u){
#ifdef ENV_PAIR_DIAGNOSTICS
   if(r==0u)++outputPair.counts0.z;else ++outputPair.counts1.z;outputPair.order[r]=envPairDigest(outputPair.order[r],2u,node.links.z,0u);
#endif
   if(any(isnan(inst.inverse0))||any(isinf(inst.inverse0))||any(isnan(inst.inverse1))||any(isinf(inst.inverse1))||any(isnan(inst.inverse2))||any(isinf(inst.inverse2))){outputPair.status[r]=-1;live&=~(1u<<r);bm&=~(1u<<r);continue;}
   float inverseDet=dot(inst.inverse0.xyz,cross(inst.inverse1.xyz,inst.inverse2.xyz));
   if(!finite1(inverseDet)||inverseDet==0.){outputPair.status[r]=-1;live&=~(1u<<r);bm&=~(1u<<r);continue;}
   localOrigin[r]=vec3(dot(inst.inverse0,vec4(origin[r],1)),dot(inst.inverse1,vec4(origin[r],1)),dot(inst.inverse2,vec4(origin[r],1)));
   localDirection[r]=vec3(dot(inst.inverse0.xyz,direction[r]),dot(inst.inverse1.xyz,direction[r]),dot(inst.inverse2.xyz,direction[r]));float squared=dot(localDirection[r],localDirection[r]);
   if(!finite3(localOrigin[r])||!finite3(localDirection[r])||!finite1(squared)||squared<=0.){outputPair.status[r]=-1;live&=~(1u<<r);bm&=~(1u<<r);}}
  if(bm==0u)continue;uint bs[64];uvec4 bits=uvec4(0);uint bp=1u;bs[0]=inst.identity.x;envPairMaskSet(bits,0u,bm);uvec2 bPending=uvec2(0),bVisited=uvec2(0);for(uint r=0u;r<2u;++r)if((bm&(1u<<r))!=0u)bPending[r]=1u;
  while(bp!=0u && (bm&live)!=0u){--bp;uint bat=bs[bp],laneMask=envPairMaskGet(bits,bp)&bm&live;if(laneMask==0u)continue;
   for(uint r=0u;r<2u;++r)if((laneMask&(1u<<r))!=0u){if(bVisited[r]++>=geometryBottom.data.length()){outputPair.status[r]=-1;live&=~(1u<<r);laneMask&=~(1u<<r);}else{--bPending[r];
#ifdef ENV_PAIR_DIAGNOSTICS
    if(r==0u)++outputPair.counts0.y;else ++outputPair.counts1.y;outputPair.order[r]=envPairDigest(outputPair.order[r],3u,bat,node.links.z);
#endif
   }}
   if(laneMask==0u)continue;if(bat>=geometryBottom.data.length()){for(uint r=0u;r<2u;++r)if((laneMask&(1u<<r))!=0u){outputPair.status[r]=-1;live&=~(1u<<r);}continue;}
   GeometryNode child=geometryBottom.data[bat];
#ifdef ENV_PAIR_DIAGNOSTICS
   ++outputPair.nodeLoads;
#endif
   for(uint r=0u;r<2u;++r)if((laneMask&(1u<<r))!=0u && !environmentNodeValid(child)){outputPair.status[r]=-1;live&=~(1u<<r);laneMask&=~(1u<<r);}
   if(laneMask==0u)continue;
   uint bh=0u;for(uint r=0u;r<2u;++r)if((laneMask&(1u<<r))!=0u && geometryBox(child,localOrigin[r],localDirection[r],0.,farT[r]))bh|=1u<<r;if(bh==0u)continue;
   if(child.links.w==0u){uint push=bh;for(uint r=0u;r<2u;++r)if((bh&(1u<<r))!=0u){if(bPending[r]+2u>64u||child.links.x>=geometryBottom.data.length()||child.links.y>=geometryBottom.data.length()){outputPair.status[r]=-1;live&=~(1u<<r);push&=~(1u<<r);}else bPending[r]+=2u;}
    if(push!=0u){if(bp+2u>uint(ENV_PAIR_PHYSICAL_LIMIT))return false;bs[bp]=child.links.x;envPairMaskSet(bits,bp++,push);bs[bp]=child.links.y;envPairMaskSet(bits,bp++,push);}continue;}
   if(child.links.z>geometryTriangles.data.length()||child.links.w>geometryTriangles.data.length()-child.links.z){for(uint r=0u;r<2u;++r)if((bh&(1u<<r))!=0u){outputPair.status[r]=-1;live&=~(1u<<r);}continue;}
   for(uint i=0u;i<child.links.w && (bh&live)!=0u;++i){GeometryTriangle tri=geometryTriangles.data[child.links.z+i];
#ifdef ENV_PAIR_DIAGNOSTICS
    ++outputPair.triangleLoads;
#endif
    for(uint r=0u;r<2u;++r)if((bh&live&(1u<<r))!=0u){
#ifdef ENV_PAIR_DIAGNOSTICS
     if(r==0u)++outputPair.counts0.w;else ++outputPair.counts1.w;outputPair.order[r]=envPairDigest(outputPair.order[r],4u,child.links.z+i,node.links.z);
#endif
     if(!finite3(tri.a.xyz)||!finite3(tri.e1.xyz)||!finite3(tri.e2.xyz)){outputPair.status[r]=-1;live&=~(1u<<r);continue;}
     vec3 p=cross(localDirection[r],tri.e2.xyz);float det=dot(tri.e1.xyz,p);if(!finite1(det)){outputPair.status[r]=-1;live&=~(1u<<r);continue;}if(det==0.)continue;
     vec3 delta=localOrigin[r]-tri.a.xyz;float u=dot(delta,p)/det;if(!finite1(u)){outputPair.status[r]=-1;live&=~(1u<<r);continue;}if(u<0.||u>1.)continue;
     vec3 q=cross(delta,tri.e1.xyz);float v=dot(localDirection[r],q)/det;if(!finite1(v)){outputPair.status[r]=-1;live&=~(1u<<r);continue;}if(v<0.||u+v>1.)continue;
     vec3 normal=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);float planeDet=dot(localDirection[r],normal);if(!finite3(normal)||!finite1(planeDet)||planeDet==0.){outputPair.status[r]=-1;live&=~(1u<<r);continue;}
     float t=dot(tri.a.xyz-localOrigin[r],normal)/planeDet;if(!finite1(t)){outputPair.status[r]=-1;live&=~(1u<<r);continue;}if(t<0.||t>farT[r])continue;
     if(t<=2e-5||t>=farT[r]-2e-5){endpoint[r]=true;continue;}outputPair.status[r]=1;live&=~(1u<<r);
    }
   }
  }
 }
 for(uint r=0u;r<2u;++r)if((live&(1u<<r))!=0u && endpoint[r])outputPair.status[r]=-1;
 return true;
}
