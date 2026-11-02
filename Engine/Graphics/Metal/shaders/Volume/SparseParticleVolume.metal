#pragma once
bool sparseLess(int3 a, int3 b) {
    return a.x<b.x || (a.x==b.x && (a.y<b.y || (a.y==b.y && a.z<b.z)));
}
float sparseVoxel(int3 v, texture3d<float, access::sample> atlas, device const int4* words) {
    int3 b=int3(floor(float3(v)/8.0));
    int lo=0, hi=words[0].x;
    while(lo<hi) { int mid=(lo+hi)/2;
        if(sparseLess(words[4+mid].xyz,b)) lo=mid+1; else hi=mid;
    }
    if(lo>=words[0].x || any(words[4+lo].xyz!=b)) return 0.0;
    int n=words[0].y, t=words[4+lo].w;
    int3 tile=int3(t%n,(t/n)%n,t/(n*n));
    return atlas.read(uint3(tile*8+v-b*8)).r;
}
float sparseDensity(float3 world, texture3d<float, access::sample> atlas, device const int4* words) {
    if(words[0].x==0) return 0.0;
    float3 p=world/as_type<float>(words[1].x)-0.5;
    // Reject before conversion to integer; avoid overflow for distant cameras.
    if(any(p<float3(words[2].xyz)*8.0-1.0) ||
       any(p>float3(words[3].xyz+1)*8.0)) return 0.0;
    int3 v=int3(floor(p)); float3 f=fract(p); float d=0.0;
    for(int k=0;k<8;++k) {
        int3 o=int3(k&1,(k>>1)&1,(k>>2)&1);
        float3 w=mix(float3(1.0)-f,f,float3(o));
        d+=sparseVoxel(v+o, atlas, words)*w.x*w.y*w.z;
    }
    return d;
}
