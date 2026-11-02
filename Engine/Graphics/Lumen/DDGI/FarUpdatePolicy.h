#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace primal::graphics::lumen {
// Transactional CPU selection only. Commit means commands were recorded, never
// GPU completion. The caller owns source readiness, uploads and fenced slot reuse.
class FarUpdatePolicy {
public:
    struct Plan { std::vector<uint32_t> indices; uint64_t epoch=0; uint32_t rotation=0; bool bootstrap=false; };
    void Initialize(uint32_t total, uint32_t budget) {
        total_=total; budget_=budget; last_.assign(total,0); Reset();
    }
    void Reset() { ++epoch_; bootstrap_=true; rotation_=0; serial_=0; std::fill(last_.begin(),last_.end(),0); }
    Plan Select(bool sourceReady, bool warmup, bool enabled=true) const {
        Plan p; p.epoch=epoch_; p.rotation=rotation_; p.bootstrap=bootstrap_;
        if (!sourceReady || warmup || !enabled || !total_ || !budget_) return p;
        const uint32_t count=bootstrap_?total_:std::min(budget_,total_);
        p.indices.reserve(count);
        for(uint32_t i=0;i<count;++i) p.indices.push_back(bootstrap_?i:(rotation_+i)%total_);
        return p;
    }
    bool Commit(const Plan& p, bool uploaded, bool recorded) {
        if(!uploaded || !recorded || p.indices.empty() || p.epoch!=epoch_ || p.rotation!=rotation_ || p.bootstrap!=bootstrap_) return false;
        const auto expected=Select(true,false);
        if(p.indices!=expected.indices) return false;
        ++serial_;
        for(auto i:p.indices) last_[i]=serial_;
        rotation_=p.bootstrap?0:(rotation_+uint32_t(p.indices.size()))%total_;
        bootstrap_=false;
        return true;
    }
    static uint64_t Hash(const Plan& p) {
        uint64_t h=1469598103934665603ull;
        for(auto i:p.indices) for(unsigned b=0;b<4;++b) {h^=(i>>(b*8))&255u;h*=1099511628211ull;}
        return h;
    }
    uint32_t MaxAge() const { uint32_t age=0; for(auto f:last_) age=std::max(age,serial_-f);return age; }
    uint64_t Epoch() const{return epoch_;}
    uint32_t Rotation() const{return rotation_;}
    uint32_t Budget() const{return budget_;}
    bool BootstrapPending() const{return bootstrap_;}
private:
    uint32_t total_=0,budget_=0,rotation_=0,serial_=0;
    uint64_t epoch_=0;
    bool bootstrap_=true;
    std::vector<uint32_t> last_;
};
}
