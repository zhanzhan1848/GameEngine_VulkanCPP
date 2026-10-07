#ifndef SC_BOUNDARY_BINARY_INSERT
#define SC_BOUNDARY_BINARY_INSERT false
#endif
// Requires readonly uint scBoundaryWords[]. Returns exact nearest 32 when the
// bounded traversal completes; malformed data/budget exhaustion fail closed.
bool scBoundaryFinite(vec3 v) { return !any(isnan(v)) && !any(isinf(v)); }
bool scBoundaryNearest(uint card,uint triangle,uint epoch,vec3 query,
                       out uint offsets[32],out uint count) {
    count=0u;
    uint length=uint(scBoundaryWords.length());
    if(length<8u || scBoundaryWords[0]!=0x53434b31u || scBoundaryWords[1]!=epoch ||
       scBoundaryWords[3]!=length || length>4194304u || !scBoundaryFinite(query))return false;
    uint cards=scBoundaryWords[2],nb=scBoundaryWords[4],pb=scBoundaryWords[5];
    if(cards==0u || cards>65536u || card>=cards || 8u+2u*cards>nb || nb>pb || pb>length ||
       scBoundaryWords[6]!=(pb-nb)/12u || (pb-nb)%12u!=0u ||
       scBoundaryWords[7]!=(length-pb)/4u || (length-pb)%4u!=0u)return false;
    uint records=scBoundaryWords[8u+2u*card],n=scBoundaryWords[9u+2u*card];
    if(n==0u || n>8192u || records<8u+2u*cards || records>nb || n>(nb-records)/4u)return false;
    uint lo=0u,hi=n;
    for(uint step=0u;step<14u && lo<hi;++step){uint mid=(lo+hi)/2u;
        if(scBoundaryWords[records+4u*mid]<triangle)lo=mid+1u;else hi=mid;}
    if(lo>=n || scBoundaryWords[records+4u*lo]!=triangle)return false;
    uint r=records+4u*lo;
    if(scBoundaryWords[r+2u]==0u || scBoundaryWords[r+2u]>8192u)return false;
    uint stack[64];uint pending=1u;stack[0]=scBoundaryWords[r+1u];
    float distances[32];
    for(uint visited=0u;visited<256u && pending>0u;++visited) {
        uint node=stack[--pending];
        if(node<nb || node>pb || pb-node<12u || (node-nb)%12u!=0u){count=0u;return false;}
        vec3 a=uintBitsToFloat(uvec3(scBoundaryWords[node],scBoundaryWords[node+1u],scBoundaryWords[node+2u]));
        vec3 b=uintBitsToFloat(uvec3(scBoundaryWords[node+4u],scBoundaryWords[node+5u],scBoundaryWords[node+6u]));
        if(!scBoundaryFinite(a) || !scBoundaryFinite(b) || any(greaterThan(a,b))){count=0u;return false;}
        vec3 delta=max(max(a-query,query-b),vec3(0));float bound=dot(delta,delta);
        // Inflate pruning slightly to avoid rounding away an equally close point.
        if(count==32u && bound>distances[31]+max(1e-6,distances[31]*1e-5))continue;
        uint entries=scBoundaryWords[node+9u];
        if(entries>0u){
            uint at=scBoundaryWords[node+8u];
            if(entries>8u || at<pb || at>length || (at-pb)%4u!=0u || entries>(length-at)/4u){count=0u;return false;}
            for(uint i=0u;i<entries;++i){uint p=at+4u*i;
                vec3 position=uintBitsToFloat(uvec3(scBoundaryWords[p],scBoundaryWords[p+1u],scBoundaryWords[p+2u]));
                if(!scBoundaryFinite(position)){count=0u;return false;}
                vec3 d=position-query;float distance=dot(d,d);
                if(isnan(distance)||isinf(distance)){count=0u;return false;}
                uint slot=count;
                // Same strict tuple predicate as the linear scan: equal
                // distance/source tuples stay before this newly visited point.
                // Float distance formation and all writes/traversal stay intact.
                if(SC_BOUNDARY_BINARY_INSERT){uint begin=0u,end=count;
                    while(begin<end){uint mid=(begin+end)/2u;
                        if(distance<distances[mid] || (distance==distances[mid] &&
                           scBoundaryWords[p+3u]<scBoundaryWords[offsets[mid]+3u]))end=mid;
                        else begin=mid+1u;
                    }
                    slot=begin;
                }else{
                for(uint j=0u;j<count;++j)if(distance<distances[j] ||
                    (distance==distances[j] && scBoundaryWords[p+3u]<scBoundaryWords[offsets[j]+3u])){slot=j;break;}
                }
                if(slot<32u){uint last=min(count,31u);
                    for(uint j=last;j>slot;--j){distances[j]=distances[j-1u];offsets[j]=offsets[j-1u];}
                    distances[slot]=distance;offsets[slot]=p;count=min(count+1u,32u);}
            }
        }else{
            if(pending>61u){count=0u;return false;}
            uint left=scBoundaryWords[node+3u],right=scBoundaryWords[node+7u];
            // The builder emits forward-only children, also rejecting cycles.
            if(left<=node || right<=node){count=0u;return false;}
            if(left<nb || right<nb || left>pb || right>pb || pb-left<12u || pb-right<12u ||
               (left-nb)%12u!=0u || (right-nb)%12u!=0u){count=0u;return false;}
            vec3 llo=uintBitsToFloat(uvec3(scBoundaryWords[left],scBoundaryWords[left+1u],scBoundaryWords[left+2u]));
            vec3 lhi=uintBitsToFloat(uvec3(scBoundaryWords[left+4u],scBoundaryWords[left+5u],scBoundaryWords[left+6u]));
            vec3 rlo=uintBitsToFloat(uvec3(scBoundaryWords[right],scBoundaryWords[right+1u],scBoundaryWords[right+2u]));
            vec3 rhi=uintBitsToFloat(uvec3(scBoundaryWords[right+4u],scBoundaryWords[right+5u],scBoundaryWords[right+6u]));
            vec3 ld=max(max(llo-query,query-lhi),vec3(0)),rd=max(max(rlo-query,query-rhi),vec3(0));
            bool leftFirst=dot(ld,ld)<=dot(rd,rd);
            stack[pending++]=leftFirst?right:left;stack[pending++]=leftFirst?left:right;
        }
    }
    if(pending>0u){count=0u;return false;}
    return count>0u;
}
