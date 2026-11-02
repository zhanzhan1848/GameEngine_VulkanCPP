#include "Engine/Graphics/Lumen/DDGI/FarUpdatePolicy.h"
#include <cstdio>
#include <set>
using primal::graphics::lumen::FarUpdatePolicy;
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
    for(unsigned budget:{512u,2048u}) {
        FarUpdatePolicy p;p.Initialize(2048,budget);
        const auto epoch=p.Epoch();
        CHECK(p.Select(false,false).indices.empty());CHECK(p.Select(true,true).indices.empty());
        CHECK(p.Select(true,false,false).indices.empty());CHECK(p.BootstrapPending());
        auto boot=p.Select(true,false);CHECK(boot.bootstrap&&boot.indices.size()==2048);
        CHECK(!p.Commit(boot,false,true));CHECK(!p.Commit(boot,true,false));CHECK(p.BootstrapPending());
        CHECK(p.Commit(boot,true,true));CHECK(!p.BootstrapPending());CHECK(p.Rotation()==0);
        CHECK(!p.Commit(boot,true,true));
        std::set<unsigned> covered;
        for(unsigned frame=0;frame<4;++frame) {
            auto next=p.Select(true,false);CHECK(next.indices.size()==budget);
            CHECK(std::set<unsigned>(next.indices.begin(),next.indices.end()).size()==next.indices.size());
            auto bad=next;bad.indices[0]=2048;CHECK(!p.Commit(bad,true,true));
            CHECK(p.Select(false,false).indices.empty());CHECK(p.Select(true,true).indices.empty());
            CHECK(p.Select(true,false).indices==next.indices);
            CHECK(!p.Commit(next,false,false));CHECK(p.Commit(next,true,true));
            covered.insert(next.indices.begin(),next.indices.end());CHECK(p.MaxAge()<=3);
        }
        CHECK(covered.size()==2048);CHECK(p.Rotation()==0);
        auto stale=p.Select(true,false);p.Reset();CHECK(p.Epoch()==epoch+1);
        CHECK(!p.Commit(stale,true,true));CHECK(p.BootstrapPending());
        CHECK(p.Commit(p.Select(true,false),true,true));CHECK(p.MaxAge()==0);
    }
    // General non-divisible grids wrap uniquely without starving the tail.
    FarUpdatePolicy p;p.Initialize(1031,512);CHECK(p.Commit(p.Select(true,false),true,true));
    std::set<unsigned> covered;
    for(unsigned frame=0;frame<3;++frame) {
        auto plan=p.Select(true,false);CHECK(plan.indices.size()==512);
        covered.insert(plan.indices.begin(),plan.indices.end());CHECK(p.Commit(plan,true,true));CHECK(p.MaxAge()<=2);
    }
    CHECK(covered.size()==1031);
    p.Initialize(17,512);CHECK(p.Commit(p.Select(true,false),true,true));
    CHECK(p.Select(true,false).indices.size()==17);
    p.Initialize(0,512);CHECK(p.Select(true,false).indices.empty());
    std::printf("DDGI far policy: %u checks passed\n",checks);return 0;
}
