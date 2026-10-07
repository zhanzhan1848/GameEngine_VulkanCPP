#pragma once
#include <array>
#include <cstdint>
namespace primal::graphics::rhi {
// Render-thread contract: publish only after the host's slot fence succeeded.
// A frame number or device BeginFrame by itself never proves completion.
class FrameSlotCompletion {
    std::array<uint64_t,3> serials_{};
    uint32_t active_=UINT32_MAX;
public:
    void Invalidate(){active_=UINT32_MAX;}
    bool Publish(uint32_t slot){
        if(slot>=serials_.size() || serials_[slot]==UINT64_MAX){Invalidate();return false;}
        ++serials_[slot];active_=slot;return true;
    }
    uint64_t Serial(uint32_t slot)const{return slot<serials_.size()?serials_[slot]:0;}
    bool CanReuse(uint32_t slot,uint64_t previous)const{
        return slot<serials_.size() && active_==slot && serials_[slot]>previous;
    }
};
}
