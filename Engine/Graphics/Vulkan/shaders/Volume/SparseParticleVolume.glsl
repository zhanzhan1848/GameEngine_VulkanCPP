// Sparse brick atlas: zero for missing bricks; interpolation crosses brick boundaries.
layout(set=0, binding=SPARSE_ATLAS_BINDING) uniform texture3D particle_atlas;
layout(std430, set=0, binding=SPARSE_DIRECTORY_BINDING) readonly buffer ParticleDirectory { ivec4 particle_words[]; };
bool sparseLess(ivec3 a, ivec3 b) {
    return a.x<b.x || (a.x==b.x && (a.y<b.y || (a.y==b.y && a.z<b.z)));
}
float sparseVoxel(ivec3 v) {
    ivec3 b=ivec3(floor(vec3(v)/8.0));
    int lo=0, hi=particle_words[0].x;
    while(lo<hi) { int mid=(lo+hi)/2;
        if(sparseLess(particle_words[4+mid].xyz,b)) lo=mid+1; else hi=mid;
    }
    if(lo>=particle_words[0].x || any(notEqual(particle_words[4+lo].xyz,b))) return 0.0;
    int n=particle_words[0].y, t=particle_words[4+lo].w;
    ivec3 tile=ivec3(t%n,(t/n)%n,t/(n*n));
    return texelFetch(particle_atlas,tile*8+v-b*8,0).r;
}
float sparseDensity(vec3 world) {
    if(particle_words[0].x==0) return 0.0;
    vec3 p=world/intBitsToFloat(particle_words[1].x)-0.5;
    // Reject before conversion to integer; avoid overflow for distant cameras.
    if(any(lessThan(p,vec3(particle_words[2].xyz)*8.0-1.0)) ||
       any(greaterThan(p,vec3(particle_words[3].xyz+1)*8.0))) return 0.0;
    ivec3 v=ivec3(floor(p)); vec3 f=fract(p); float d=0.0;
    for(int k=0;k<8;++k) {
        ivec3 o=ivec3(k&1,(k>>1)&1,(k>>2)&1);
        vec3 w=mix(vec3(1.0)-f,f,vec3(o));
        d+=sparseVoxel(v+o)*w.x*w.y*w.z;
    }
    return d;
}
