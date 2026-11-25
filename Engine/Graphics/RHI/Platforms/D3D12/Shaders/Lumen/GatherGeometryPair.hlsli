// Experimental Gather-only original-query two-ray scheduler. The caller must
// validate each endpoint and give independent start/direction/far, and must
// rerun both original queries if this function reports physical overflow.
// Requires GeometryFirstHit.glsl and DDGI_GEOMETRY_REPLACE / ANY_HIT.
#if !defined(DDGI_GEOMETRY_REPLACE) || !defined(DDGI_GEOMETRY_ANY_HIT)
#error Pair traversal requires the original any-hit replacement query
#endif
#ifndef DDGI_PAIR_PHYSICAL_LIMIT
#define DDGI_PAIR_PHYSICAL_LIMIT 64
#endif
#if DDGI_PAIR_PHYSICAL_LIMIT > 64 || DDGI_PAIR_PHYSICAL_LIMIT < 1
#error Physical pair stack limit must be within the original 64-entry allocation
#endif
struct PairTraceResult { int2 status; float2 hitT;
#ifdef DDGI_PAIR_DIAGNOSTICS
 uint4 counts0;uint4 counts1;uint2 order;uint nodeLoads;uint instanceLoads;uint triangleLoads;
#endif
};
void pairMaskSet(inout uint4 bits,uint pos,uint mask){uint i=pos>>4u,shift=(pos&15u)*2u;bits[i]=(bits[i]&~(3u<<shift))|((mask&3u)<<shift);}
uint pairMaskGet(uint4 bits,uint pos){return (bits[pos>>4u]>>((pos&15u)*2u))&3u;}
#ifdef DDGI_PAIR_DIAGNOSTICS
uint pairDigest(uint h,uint kind,uint id,uint instance){h=(h^kind)*16777619u;h=(h^id)*16777619u;return (h^instance)*16777619u;}
#endif
// Returns false only on physical union-stack overflow; results then MUST be discarded.
bool geometryFirstHitPair(float3 origin[2],float3 direction[2],float farT[2],out PairTraceResult outp) {
 outp.status=(int2)(0);outp.hitT=float2(farT[0], farT[1]);
#ifdef DDGI_PAIR_DIAGNOSTICS
 outp.counts0=(uint4)(0);outp.counts1=(uint4)(0);outp.order=(uint2)(2166136261u);outp.nodeLoads=0u;outp.instanceLoads=0u;outp.triangleLoads=0u;
#endif
 uint ts[64];uint4 tm=(uint4)(0);uint tp=1u;ts[0]=0u;pairMaskSet(tm,0u,3u);uint2 pending=(uint2)(1u);uint live=3u;
 while(tp!=0u && live!=0u){--tp;uint at=ts[tp],mask=pairMaskGet(tm,tp)&live;if(mask==0u)continue;
  for(uint r=0u;r<2u;++r)if((mask&(1u<<r))!=0u){--pending[r];
#ifdef DDGI_PAIR_DIAGNOSTICS
   if(r==0u)++outp.counts0.x;else ++outp.counts1.x;outp.order[r]=pairDigest(outp.order[r],1u,at,0u);
#endif
  }
  if(at>=(byteSize(geometryTop) / 48u)){for(uint r=0u;r<2u;++r)if((mask&(1u<<r))!=0u){outp.status[r]=-1;live&=~(1u<<r);}continue;}
  GeometryNode node=geometryTop.Load<GeometryNode>((at) * 48u);
#ifdef DDGI_PAIR_DIAGNOSTICS
  ++outp.nodeLoads;
#endif
uint hits=0u;
  for(uint r=0u;r<2u;++r)if((mask&(1u<<r))!=0u && geometryBox(node,origin[r],direction[r],0.,farT[r]))hits|=1u<<r;
  if(hits==0u)continue;
  if(node.links.w==0u){uint pushMask=hits;
   for(uint r=0u;r<2u;++r)if((hits&(1u<<r))!=0u){if(pending[r]+2u>64u){outp.status[r]=-1;live&=~(1u<<r);pushMask&=~(1u<<r);}else pending[r]+=2u;}
   if(pushMask!=0u){if(tp+2u>uint(DDGI_PAIR_PHYSICAL_LIMIT))return false;ts[tp]=node.links.x;pairMaskSet(tm,tp++,pushMask);ts[tp]=node.links.y;pairMaskSet(tm,tp++,pushMask);}continue;
  }
  if(node.links.z>=(byteSize(geometryInstances) / 64u)){for(uint r=0u;r<2u;++r)if((hits&(1u<<r))!=0u){outp.status[r]=-1;live&=~(1u<<r);}continue;}
  GeometryInstance inst=geometryInstances.Load<GeometryInstance>((node.links.z) * 64u);
#ifdef DDGI_PAIR_DIAGNOSTICS
  ++outp.instanceLoads;
#endif
float3 lo[2],ld[2];uint bottomMask=hits;
  for(uint r=0u;r<2u;++r)if((hits&(1u<<r))!=0u){
#ifdef DDGI_PAIR_DIAGNOSTICS
   if(r==0u)++outp.counts0.z;else ++outp.counts1.z;outp.order[r]=pairDigest(outp.order[r],2u,node.links.z,0u);
#endif
   lo[r]=float3(dot(inst.inverse0,float4(origin[r], 1)), dot(inst.inverse1,float4(origin[r], 1)), dot(inst.inverse2,float4(origin[r], 1)));
   ld[r]=float3(dot(inst.inverse0.xyz,direction[r]), dot(inst.inverse1.xyz,direction[r]), dot(inst.inverse2.xyz,direction[r]));
   if(any(isnan(lo[r]))||any(isinf(lo[r]))||any(isnan(ld[r]))||any(isinf(ld[r]))){outp.status[r]=-1;live&=~(1u<<r);bottomMask&=~(1u<<r);}
  }
  if(bottomMask==0u)continue;
  uint bs[64];uint4 bm=(uint4)(0);uint bp=1u;bs[0]=inst.identity.x;pairMaskSet(bm,0u,bottomMask);uint2 bPending=(uint2)(0u);for(uint r=0u;r<2u;++r)if((bottomMask&(1u<<r))!=0u)bPending[r]=1u;
  while(bp!=0u && (bottomMask&live)!=0u){--bp;uint bat=bs[bp],m=pairMaskGet(bm,bp)&bottomMask&live;if(m==0u)continue;
   for(uint r=0u;r<2u;++r)if((m&(1u<<r))!=0u){--bPending[r];
#ifdef DDGI_PAIR_DIAGNOSTICS
    if(r==0u)++outp.counts0.y;else ++outp.counts1.y;outp.order[r]=pairDigest(outp.order[r],3u,bat,node.links.z);
#endif
  }
   if(bat>=(byteSize(geometryBottom) / 48u)){for(uint r=0u;r<2u;++r)if((m&(1u<<r))!=0u){outp.status[r]=-1;live&=~(1u<<r);}continue;}
   GeometryNode child=geometryBottom.Load<GeometryNode>((bat) * 48u);
#ifdef DDGI_PAIR_DIAGNOSTICS
  ++outp.nodeLoads;
#endif
uint bhits=0u;
   for(uint r=0u;r<2u;++r)if((m&(1u<<r))!=0u && geometryBox(child,lo[r],ld[r],0.,farT[r]))bhits|=1u<<r;
   if(bhits==0u)continue;
   if(child.links.w==0u){uint pushMask=bhits;
    for(uint r=0u;r<2u;++r)if((bhits&(1u<<r))!=0u){if(bPending[r]+2u>64u){outp.status[r]=-1;live&=~(1u<<r);pushMask&=~(1u<<r);}else bPending[r]+=2u;}
    if(pushMask!=0u){if(bp+2u>uint(DDGI_PAIR_PHYSICAL_LIMIT))return false;bs[bp]=child.links.x;pairMaskSet(bm,bp++,pushMask);bs[bp]=child.links.y;pairMaskSet(bm,bp++,pushMask);}continue;
   }
   if(child.links.z>(byteSize(geometryTriangles) / 48u) || child.links.w>(byteSize(geometryTriangles) / 48u)-child.links.z){for(uint r=0u;r<2u;++r)if((bhits&(1u<<r))!=0u){outp.status[r]=-1;live&=~(1u<<r);}continue;}
   for(uint i=0u;i<child.links.w && (bhits&live)!=0u;++i){GeometryTriangle tri=geometryTriangles.Load<GeometryTriangle>((child.links.z+i) * 48u);
#ifdef DDGI_PAIR_DIAGNOSTICS
  ++outp.triangleLoads;
#endif

    for(uint r=0u;r<2u;++r)if((bhits&live&(1u<<r))!=0u){
#ifdef DDGI_PAIR_DIAGNOSTICS
     if(r==0u)++outp.counts0.w;else ++outp.counts1.w;outp.order[r]=pairDigest(outp.order[r],4u,child.links.z+i,node.links.z);
#endif
     float3 p=cross(ld[r],tri.e2.xyz);float det=dot(tri.e1.xyz,p);
     float3 localN=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);float3 worldN=inst.inverse0.xyz*localN.x+inst.inverse1.xyz*localN.y+inst.inverse2.xyz*localN.z;
     float worldNLength=length(worldN),facing=dot(ld[r],localN);
     if(any(isnan(localN))||any(isinf(localN))||any(isnan(worldN))||any(isinf(worldN))||isnan(worldNLength)||isinf(worldNLength)||!(worldNLength>0.)||isnan(facing)||isinf(facing)||isnan(det)||isinf(det)||det==0.||abs(facing)<=1e-6*worldNLength){outp.status[r]=-1;live&=~(1u<<r);continue;}
     if(det==0.)continue;float3 v=lo[r]-tri.a.xyz;float u=dot(v,p)/det;if(u<0.||u>1.)continue;
     float3 q=cross(v,tri.e1.xyz);float bary=dot(ld[r],q)/det;float3 normal=localN;float planeDet=dot(ld[r],normal);
     if(planeDet==0.){outp.status[r]=-1;live&=~(1u<<r);continue;}
     float t=dot(tri.a.xyz-lo[r],normal)/planeDet;
     if(isnan(t)||isinf(t)||isnan(bary)||isinf(bary)||isnan(u)||isinf(u)){outp.status[r]=-1;live&=~(1u<<r);continue;}
     if(bary<0.||u+bary>1.||t<0.||t>farT[r])continue;
     outp.status[r]=1;outp.hitT[r]=t;live&=~(1u<<r);
    }
   }
  }
 }
 return true;
}
