// Private proposal + per-descendant shader-box oracle. Not a distance-only certificate.
layout(set=0,binding=17,std430) readonly buffer EnvironmentLeafField {uint words[];} leafField;
#ifdef ENV_FIELD_AUDIT
uint fieldAttempts=0u,fieldProposals=0u,fieldOracleBoxes=0u,fieldRejects=0u,fieldSkips=0u,fieldReads=0u;
#endif
uint fieldRead(uint at){
#ifdef ENV_FIELD_AUDIT
 ++fieldReads;
#endif
 return leafField.words[at];
}
bool fieldProposeAndVerify(uint root,uint node,GeometryNode current,vec3 origin,vec3 direction,float farT){
#ifdef ENV_FIELD_AUDIT
 ++fieldAttempts;
#endif
 uint len=uint(leafField.words.length());if(len<32u||fieldRead(0)!=0x464c4546u||fieldRead(1)!=1u||fieldRead(2)!=len||fieldRead(3)!=geometryBottom.data.length()||fieldRead(7)!=2u)return false;
 uint m=0xffffffffu;for(uint j=0u;j<2u;++j){uint q=8u+j*12u;if(fieldRead(q)==root)m=q;}if(m==0xffffffffu)return false;
 uint map=fieldRead(4);if(map>len || node>=geometryBottom.data.length() || node>(len-map)/2u || map+node*2u+1u>=len)return false;
 uint start=fieldRead(map+node*2u),count=fieldRead(map+node*2u+1u),leafBase=fieldRead(5),leafLen=fieldRead(6);
 if(start==0xffffffffu||count==0u||count>32u||start>leafLen||count>leafLen-start||leafBase>len||leafLen>len-leafBase)return false;
 if(!finite3(origin)||!finite3(direction)||!finite1(farT)||farT<=0.)return false;
 float lo=0.,hi=farT;for(uint k=0u;k<3u;++k){if(abs(direction[k])<1e-20)return false;float a=(current.lo[k]-origin[k])/direction[k],b=(current.hi[k]-origin[k])/direction[k];if(!finite1(a)||!finite1(b))return false;lo=max(lo,min(a,b));hi=min(hi,max(a,b));if(lo>hi)return false;}
 vec3 low=vec3(uintBitsToFloat(fieldRead(m+2u)),uintBitsToFloat(fieldRead(m+3u)),uintBitsToFloat(fieldRead(m+4u)));vec3 step=vec3(uintBitsToFloat(fieldRead(m+5u)),uintBitsToFloat(fieldRead(m+6u)),uintBitsToFloat(fieldRead(m+7u)));
 if(!finite3(low)||!finite3(step)||any(lessThanEqual(step,vec3(0)))||fieldRead(m+8u)!=16u)return false;
 vec3 cell=(origin+direction*(lo+(hi-lo)*.5)-low)/step;if(!finite3(cell)||any(lessThan(cell,vec3(0)))||any(greaterThanEqual(cell,vec3(16))))return false;
 uvec3 ijk=uvec3(cell);uint grid=fieldRead(m+1u),at=ijk.x+16u*ijk.y+256u*ijk.z;if(grid>len||at>=len-grid||uintBitsToFloat(fieldRead(grid+at))<=0.)return false;
#ifdef ENV_FIELD_AUDIT
 ++fieldProposals;
#endif
 for(uint j=0u;j<count;++j){uint index=fieldRead(leafBase+start+j);if(index>=geometryBottom.data.length())return false;GeometryNode leaf=geometryBottom.data[index];if(!environmentNodeValid(leaf)||leaf.links.w==0u)return false;
#ifdef ENV_FIELD_AUDIT
  ++fieldOracleBoxes;
#endif
  if(geometryBox(leaf,origin,direction,0.,farT)){
#ifdef ENV_FIELD_AUDIT
   ++fieldRejects;
#endif
   return false;}}
#ifdef ENV_FIELD_AUDIT
 ++fieldSkips;
#endif
 return true;
}
