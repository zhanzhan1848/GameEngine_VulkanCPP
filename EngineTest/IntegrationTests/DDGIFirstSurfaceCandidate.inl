// Default-off test-scene distance candidate BVH. No material sampling changes.
static void BuildDDGIFirstSurfaceBVH(const std::vector<SCProofTriangle>& scene,
    std::vector<SCProofTriangle>& triangles, std::vector<SCProofNode>& nodes) {
    triangles=scene;
    nodes.clear();
    auto bounds=[](const SCProofTriangle& t,u32 k,bool maximum) {
        const float b=t.a[k]+t.e1[k], c=t.a[k]+t.e2[k];
        // Separate coordinate rounding slack; never enlarges the hit band.
        return maximum ? std::max({t.a[k],b,c})+1e-4f : std::min({t.a[k],b,c})-1e-4f;
    };
    auto build=[&](auto&& self,u32 begin,u32 end)->u32 {
        const u32 at=u32(nodes.size()); SCProofNode node{};
        for (u32 k=0;k<3;++k) {
            node.lo[k]=std::numeric_limits<float>::max(); node.hi[k]=-node.lo[k];
            for (u32 i=begin;i<end;++i) {
                node.lo[k]=std::min(node.lo[k],bounds(triangles[i],k,false));
                node.hi[k]=std::max(node.hi[k],bounds(triangles[i],k,true));
            }
        }
        nodes.push_back(node);
        if (end-begin<=16) { nodes[at].links[2]=begin; nodes[at].links[3]=end-begin; }
        else {
            u32 axis=0;
            for (u32 k=1;k<3;++k) if (node.hi[k]-node.lo[k]>node.hi[axis]-node.lo[axis]) axis=k;
            const u32 middle=begin+(end-begin)/2;
            std::nth_element(triangles.begin()+begin,triangles.begin()+middle,triangles.begin()+end,
                [&](const auto& a,const auto& b) {
                    const float ca=2*a.a[axis]+a.e1[axis]+a.e2[axis], cb=2*b.a[axis]+b.e1[axis]+b.e2[axis];
                    return ca==cb ? a.a[3]<b.a[3] : ca<cb;
                });
            const auto left=self(self,begin,middle), right=self(self,middle,end);
            nodes[at].links[0]=left; nodes[at].links[1]=right;
        }
        return at;
    };
    build(build,0,u32(triangles.size()));
}
