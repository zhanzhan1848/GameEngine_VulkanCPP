#ifndef SC_SOURCE_BINARY_INSERT
#define SC_SOURCE_BINARY_INSERT false
#endif
// Bounded same-card/original-primitive support. Bindings are supplied by caller:
// scBoundaryWords, boundaryPoints, boundaryCache, lookupCards, lookupTriangles,
// lookupDepth and lookupLighting. Cache entries must be current-frame valid.
#include "BoundaryPointIndex.glsl"
float scBoundaryCross(vec2 a,vec2 b){return a.x*b.y-a.y*b.x;}
vec2 scBoundaryProject(vec3 p,uint axis){return axis==0u?p.yz:axis==1u?p.xz:p.xy;}
bool scBoundarySource(uint source,uint card,uint triangle,uint epoch,uint frame,vec3 indexed,
                      out vec3 position,out vec3 radiance){
    position=vec3(0);radiance=vec3(0);
    if((source&0x80000000u)!=0u){uint index=source&0x7fffffffu;
        if(index>=uint(boundaryPoints.length()) || index>=uint(boundaryCache.length()))return false;
        SCBoundaryLightPoint p=boundaryPoints[index];SCBoundaryRadianceEntry c=boundaryCache[index];
        if(any(notEqual(p.identity,uvec4(card,triangle,epoch,frame))) || any(notEqual(c.identity,p.identity)) ||
           any(notEqual(c.state,uvec4(frame,epoch,1u,0u))) || c.radiance.w!=1.)return false;
        position=p.worldFootprint.xyz;radiance=c.radiance.rgb;
    }else{
        if(card>=uint(lookupCards.length()))return false;SurfaceCacheCard c=lookupCards[card];
        uvec2 xy=uvec2(source&65535u,source>>16u);ivec2 size=textureSize(lookupDepth,0);
        if(c.resolution==0u || any(greaterThanEqual(xy,uvec2(size))) ||
           any(lessThan(xy,uvec2(c.atlas_offset_x,c.atlas_offset_y))) ||
           any(greaterThanEqual(xy,uvec2(c.atlas_offset_x,c.atlas_offset_y)+uvec2(c.resolution))))return false;
        uint at=xy.y*uint(size.x)+xy.x;if(at>=uint(lookupTriangles.length()) || lookupTriangles[at]!=triangle)return false;
        float depth=texelFetch(lookupDepth,ivec2(xy),0).r;if(isnan(depth)||isinf(depth)||depth<0)return false;
        position=cardTexelToWorld(xy,depth,c);radiance=texelFetch(lookupLighting,ivec2(xy),0).rgb;
    }
    return scBoundaryFinite(position) && scBoundaryFinite(radiance) && all(greaterThanEqual(radiance,vec3(0))) &&
        scBoundaryFinite(indexed) && length(position-indexed)<=1e-5;
}
bool scBoundaryRadianceLookup(uint card,uint triangle,uint epoch,uint frame,vec3 query,
                             out vec3 result,out vec3 witness,out uvec3 source,out vec3 weights){
    result=vec3(0);witness=vec3(0);source=uvec3(0xffffffffu);weights=vec3(0);
    if(boundaryCache.length()==0 || boundaryCache[0].state.x!=frame || boundaryCache[0].state.y!=epoch)return false;
    if(card>=uint(lookupCards.length()) || triangle==0xffffffffu || !scBoundaryFinite(query))return false;
    SurfaceCacheCard c=lookupCards[card];vec2 uv;
    if((c.flags&1u)==0u || !worldToCardUVMarginPad(query,c,uv,0.,0.))return false;
    uint offsets[32],nearest;if(!scBoundaryNearest(card,triangle,epoch,query,offsets,nearest))return false;
    vec3 positions[32],values[32];vec2 projected[32];uint refs[32];uint count=0u;
    uint axis=c.axis_direction&255u;if(axis>2u)return false;
    for(uint i=0u;i<nearest;++i){uint p=offsets[i],ref=scBoundaryWords[p+3u];
        vec3 indexed=uintBitsToFloat(uvec3(scBoundaryWords[p],scBoundaryWords[p+1u],scBoundaryWords[p+2u]));
        vec3 position,value;if(!scBoundarySource(ref,card,triangle,epoch,frame,indexed,position,value))continue;
        vec2 key=scBoundaryProject(position,axis);uint at=count;
        // Upper bound of the unchanged projected x/y/raw-source tuple.
        // Equal tuples keep their previous stable order. All float formation,
        // source validation, array shifts and hull/interpolation stay intact.
        if(SC_SOURCE_BINARY_INSERT){uint begin=0u,end=count;
            while(begin<end){uint mid=(begin+end)/2u;
                if(key.x<projected[mid].x || (key.x==projected[mid].x &&
                   (key.y<projected[mid].y || (key.y==projected[mid].y && ref<refs[mid]))))end=mid;
                else begin=mid+1u;
            }
            while(at>begin){
                positions[at]=positions[at-1u];values[at]=values[at-1u];projected[at]=projected[at-1u];refs[at]=refs[at-1u];--at;
            }
        }else{
        while(at>0u && (key.x<projected[at-1u].x || (key.x==projected[at-1u].x &&
            (key.y<projected[at-1u].y || (key.y==projected[at-1u].y && ref<refs[at-1u]))))){
            positions[at]=positions[at-1u];values[at]=values[at-1u];projected[at]=projected[at-1u];refs[at]=refs[at-1u];--at;}
        }
        positions[at]=position;values[at]=value;projected[at]=key;refs[at]=ref;++count;
    }
    if(count<2u)return false;
    uint hull[64],n=0u;
    for(uint i=0u;i<count;++i){while(n>=2u && scBoundaryCross(projected[hull[n-1u]]-projected[hull[n-2u]],projected[i]-projected[hull[n-1u]])<=0.)--n;hull[n++]=i;}
    uint lower=n;
    for(int i=int(count)-2;i>=0;--i){while(n>lower && scBoundaryCross(projected[hull[n-1u]]-projected[hull[n-2u]],projected[i]-projected[hull[n-1u]])<=0.)--n;hull[n++]=uint(i);}
    if(n>1u)--n;
    vec2 q=scBoundaryProject(query,axis);float best=.05000001;bool found=false;
    // Nonnegative fan triangles, followed by finite edges. No extrapolation.
    for(uint i=1u;i+1u<n;++i){uint a=hull[0],b=hull[i],d=hull[i+1u];vec2 e1=projected[b]-projected[a],e2=projected[d]-projected[a];
        float det=scBoundaryCross(e1,e2);if(abs(det)<1e-12)continue;
        vec2 delta=q-projected[a];float wb=scBoundaryCross(delta,e2)/det,wc=scBoundaryCross(e1,delta)/det;vec3 w=vec3(1.-wb-wc,wb,wc);
        if(any(lessThan(w,vec3(0))))continue;
        vec3 pos=w.x*positions[a]+w.y*positions[b]+w.z*positions[d];float error=length(pos-query);
        if(error<=best){vec3 value=w.x*values[a]+w.y*values[b]+w.z*values[d];if(!scBoundaryFinite(value))continue;
            best=error;result=value;witness=pos;source=uvec3(refs[a],refs[b],refs[d]);weights=w;found=true;}
    }
    for(uint i=0u;i<n && n>1u;++i){uint a=hull[i],b=hull[(i+1u)%n];vec3 edge=positions[b]-positions[a];float square=dot(edge,edge);if(square<=1e-12)continue;
        float t=dot(query-positions[a],edge)/square;if(t<0. || t>1.)continue;
        vec3 pos=positions[a]+edge*t;float error=length(pos-query);if(error<=best){vec3 value=(1.-t)*values[a]+t*values[b];if(!scBoundaryFinite(value))continue;
            best=error;result=value;witness=pos;source=uvec3(refs[a],refs[b],0xffffffffu);weights=vec3(1.-t,t,0);found=true;}
    }
    return found;
}
