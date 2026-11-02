#include "EngineTest/IntegrationTests/EnvironmentSAHInterleavedHistory.h"
#include <cassert>
#include <cstdint>
struct Completions {
    uint64_t s[3]{}; int active=-1;
    bool CanReuse(uint32_t slot,uint64_t prev) const {return active==int(slot)&&s[slot]>prev;}
    uint64_t Serial(uint32_t slot) const {return s[slot];}
    void Publish(uint32_t slot) {active=slot;++s[slot];}
};
int main(){
    EnvironmentSAHInterleavedHistory h;Completions c;
    for(unsigned i=1;i<=8;++i){auto slot=i%3;c.Publish(slot);assert(h.Observe(c,slot,i,i,0)==(i==8));}
    assert(!h.Observe(c,8%3,9,9,0));
    for(unsigned i=10;i<=17;++i){auto slot=i%3;c.Publish(slot);assert(h.Observe(c,slot,i,i,1)==(i==17));}
    c.Publish(0);assert(!h.Observe(c,0,19,18,1));assert(h.consecutive==1);
    c.Publish(1);assert(!h.Observe(c,1,20,20,1));assert(h.consecutive==1);
    c.active=-1;assert(!h.Observe(c,2,21,21,1));assert(h.consecutive==0);
    c.Publish(2);assert(!h.Observe(c,3,22,22,1));
    c.s[0]+=3;c.active=0;assert(!h.Observe(c,0,23,23,1));assert(h.consecutive==0);
}
