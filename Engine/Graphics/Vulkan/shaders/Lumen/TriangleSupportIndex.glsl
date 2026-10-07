// Internal epoch-local index; consumer declares readonly supportIndex.data[].
bool scIndexedSupport(uint card,uint triangle,out uint offset,out uint count) {
    offset=count=0u;uint size=uint(supportIndex.data.length());
    if(size<4u || supportIndex.data[0]!=0x53434831u || card>=supportIndex.data[2]) return false;
    size=min(size,supportIndex.data[3]);
    uint directory=4u+card*2u;if(directory+2u>size) return false;
    uint records=supportIndex.data[directory],number=supportIndex.data[directory+1u];
    if(number==0u || number>16384u || records>size || number>(size-records)/3u) return false;
    uint lo=0u,hi=number;
    for(uint step=0u;step<15u && lo<hi;++step) {
        uint mid=lo+(hi-lo)/2u;
        if(supportIndex.data[records+mid*3u]<triangle) lo=mid+1u;else hi=mid;
    }
    if(lo>=number || supportIndex.data[records+lo*3u]!=triangle) return false;
    offset=supportIndex.data[records+lo*3u+1u];count=supportIndex.data[records+lo*3u+2u];
    return count>=2u && count<=64u && offset<=size && count<=size-offset;
}

// Reject only a complete static-card directory; malformed/unknown data retains
// the original path. No hull-count requirement: singleton IDs remain members.
bool scTrianglePresent(uint card,uint triangle) {
    uint size=uint(supportIndex.data.length());if(size<8u)return true;
    uint footer=size-4u;if(supportIndex.data[footer]!=0x53435031u)return true;
    uint offset=supportIndex.data[footer+1u],cards=supportIndex.data[footer+2u],legacy=supportIndex.data[footer+3u];
    if(supportIndex.data[0]!=0x53434831u || cards!=supportIndex.data[2] || card>=cards ||
       legacy!=supportIndex.data[3] || offset<legacy || offset>footer || cards>footer-offset ||
       supportIndex.data[offset+card]!=1u)return true;
    uint directory=4u+card*2u;if(directory+2u>legacy)return true;
    uint records=supportIndex.data[directory],number=supportIndex.data[directory+1u];
    if(number>16384u || records>legacy || number>(legacy-records)/3u)return true;
    uint lo=0u,hi=number;
    for(uint step=0u;step<15u && lo<hi;++step){uint mid=lo+(hi-lo)/2u;
        if(supportIndex.data[records+mid*3u]<triangle)lo=mid+1u;else hi=mid;}
    return lo<number && supportIndex.data[records+lo*3u]==triangle;
}

// One owner/triangle lookup per query. Exact row span must match the already
// validated owner range. Any unsupported directory retains per-card lookup.
bool scOwnerTriangleMask(uint owner,uint first,uint last,uint triangle,out uvec2 allowed) {
    allowed=uvec2(0);uint size=uint(supportIndex.data.length());if(size<13u)return false;
    uint p=size-4u;if(supportIndex.data[p]!=0x53435031u)return false;
    uint complete=supportIndex.data[p+1u],cards=supportIndex.data[p+2u],legacy=supportIndex.data[p+3u];
    if(supportIndex.data[0]!=0x53434831u || legacy!=supportIndex.data[3] || cards!=supportIndex.data[2] ||
       complete<legacy || complete>p || cards>p-complete || complete<5u)return false;
    uint f=complete-5u;if(supportIndex.data[f]!=0x53434f31u)return false;
    uint directory=supportIndex.data[f+1u],owners=supportIndex.data[f+2u],end=supportIndex.data[f+3u];
    if(directory!=legacy || supportIndex.data[f+4u]!=legacy || end!=f || directory>end ||
       owners>(end-directory)/7u || owner>=owners)return false;
    uint row=directory+owner*7u,begin=supportIndex.data[row],span=supportIndex.data[row+1u];
    if(supportIndex.data[row+6u]!=1u || span==0u || span>64u || first!=begin || last<first ||
       last-first!=span || begin>cards || span>cards-begin)return false;
    uint record=supportIndex.data[row+4u],number=supportIndex.data[row+5u];
    if(number>65536u || record<directory+owners*7u || record>end || number>(end-record)/3u)return false;
    allowed=uvec2(supportIndex.data[row+2u],supportIndex.data[row+3u]);
    uint lo=0u,hi=number;
    for(uint step=0u;step<17u && lo<hi;++step){uint mid=lo+(hi-lo)/2u;
        if(supportIndex.data[record+mid*3u]<triangle)lo=mid+1u;else hi=mid;}
    if(lo<number && supportIndex.data[record+lo*3u]==triangle)
        allowed|=uvec2(supportIndex.data[record+lo*3u+1u],supportIndex.data[record+lo*3u+2u]);
    return true;
}
bool scTriangleCandidate(uint card,uint triangle,bool ready,uint first,uvec2 allowed) {
    uint bit=card-first;
    if(ready && card>=first && bit<64u)return ((allowed[bit>>5u]>>(bit&31u))&1u)!=0u;
    return scTrianglePresent(card,triangle);
}
