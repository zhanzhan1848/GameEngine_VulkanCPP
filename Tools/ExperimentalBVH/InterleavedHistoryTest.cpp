#include "EngineTest/IntegrationTests/EnvironmentSAHInterleavedHistory.h"
#include <cstdint>
// R3（Windows 首编）：断言改显式检查——根 CMakeLists 对 Release 全局定义
// NDEBUG，且 Clang|GNU 侧的 -UNDEBUG 对冲不覆盖 MSVC：assert 在 Windows
// Release 被编译掉，测试空转。显式检查两端全配置生效，失败时输出定位
// 信息并以退出码 1 结束（macOS 语义不变：原 assert 经 -UNDEBUG 本就恒开）。
#include <cstdio>
#include <cstdlib>
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #cond);                                \
            std::fflush(stderr);                                          \
            std::exit(1);                                                 \
        }                                                                 \
    } while (0)

struct Completions {
    uint64_t s[3]{}; int active=-1;
    bool CanReuse(uint32_t slot,uint64_t prev) const {return active==int(slot)&&s[slot]>prev;}
    uint64_t Serial(uint32_t slot) const {return s[slot];}
    void Publish(uint32_t slot) {active=slot;++s[slot];}
};
int main(){
    EnvironmentSAHInterleavedHistory h;Completions c;
    for(unsigned i=1;i<=8;++i){auto slot=i%3;c.Publish(slot);CHECK(h.Observe(c,slot,i,i,0)==(i==8));}
    CHECK(!h.Observe(c,8%3,9,9,0));
    for(unsigned i=10;i<=17;++i){auto slot=i%3;c.Publish(slot);CHECK(h.Observe(c,slot,i,i,1)==(i==17));}
    c.Publish(0);CHECK(!h.Observe(c,0,19,18,1));CHECK(h.consecutive==1);
    c.Publish(1);CHECK(!h.Observe(c,1,20,20,1));CHECK(h.consecutive==1);
    c.active=-1;CHECK(!h.Observe(c,2,21,21,1));CHECK(h.consecutive==0);
    c.Publish(2);CHECK(!h.Observe(c,3,22,22,1));
    c.s[0]+=3;c.active=0;CHECK(!h.Observe(c,0,23,23,1));CHECK(h.consecutive==0);
}
