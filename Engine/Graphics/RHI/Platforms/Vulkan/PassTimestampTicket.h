#pragma once
#include "Graphics/RHI/Core/FrameSlotCompletion.h"
#include <array>
#include <cstdint>
namespace primal::graphics::rhi::pass_timestamp {
inline bool FiniteDouble(double value){volatile uint64_t bits=std::bit_cast<uint64_t>(value);return (bits&0x7ff0000000000000ull)!=0x7ff0000000000000ull;}
struct Evaluation {bool accepted=false;const char* reason="invalid";long double nanoseconds=0;};
inline Evaluation Evaluate(int32_t result,uint64_t startAvailable,uint64_t endAvailable,
                           uint64_t startTick,uint64_t endTick,uint32_t bits,double period,long double hostBoundNs) {
    if(result!=0)return {false,"vk_result",0};
    if(!startAvailable||!endAvailable)return {false,"unavailable",0};
    if(!bits||bits>64||!FiniteDouble(period)||period<=0)return {false,"unsupported_clock",0};
    const long double cycle=std::ldexp((long double)period,int(bits));
    if(!FiniteDouble(static_cast<double>(cycle))||!FiniteDouble(static_cast<double>(hostBoundNs))||hostBoundNs<0||hostBoundNs>=cycle)return {false,"period_unbounded",0};
    const uint64_t mask=bits==64?~uint64_t(0):(uint64_t(1)<<bits)-1;
    const uint64_t a=startTick&mask,b=endTick&mask;
    if(b<a)return {false,"wrap",0};
    const long double ns=(long double)(b-a)*period;
    if(!FiniteDouble(static_cast<double>(ns)))return {false,"overflow",0};
    if(ns>hostBoundNs)return {false,"duration_exceeds_host",0};
    return {true,"accepted",ns};
}
// Numeric lifecycle ids come from successful Vulkan device initialization.
// Pointer equality alone is never a lifecycle identity.
class DeviceBinding {
public:
    bool BindOrMatch(uintptr_t address,uint64_t generation) {
        if(!address || !generation) {if(bound_)blocked_=true;return false;}
        if(blocked_)return false;
        if(!bound_) {address_=address;generation_=generation;bound_=true;return true;}
        if(address_!=address || generation_!=generation){blocked_=true;return false;}
        return true;
    }
    bool Release(uintptr_t address,uint64_t generation,bool idleSucceeded) {
        if(!bound_ || !idleSucceeded || !generation || address_!=address || generation_!=generation)return false;
        address_=0;generation_=0;bound_=false;blocked_=false;return true;
    }
    uint64_t Generation()const{return bound_?generation_:0;}
private:uintptr_t address_=0;uint64_t generation_=0;bool bound_=false,blocked_=false;
};
// Only one owner/submit per host slot. No state indicates GPU completion until
// OnFence observes a later published serial for that very slot.
class Tickets {
public:
 enum class Status {Free, Recording, Pending, Ready, Quarantine};
 struct Slot {Status status=Status::Free;uint64_t serial=0,generation=0,frame=0,command=~uint64_t(0),fence=~uint64_t(0),ticket=0;};
 bool OnFence(const FrameSlotCompletion& c,uint32_t slot,uint64_t fence){if(slot>=3||!c.CanReuse(slot,last_seen_[slot]))return false;auto serial=c.Serial(slot);last_seen_[slot]=serial;auto& s=slots_[slot];if(s.status==Status::Pending&&fence!=s.fence)s.status=Status::Quarantine;else if(s.status==Status::Pending&&serial>s.serial)s.status=Status::Ready;return true;}
 bool Start(uint32_t slot,uint64_t cmd,uint64_t fence,uint64_t frame,uint64_t ticket){if(slot>=3||!last_seen_[slot]||cmd==~uint64_t(0)||fence==~uint64_t(0)||!ticket||ticket<=last_ticket_)return false;auto& s=slots_[slot];if(s.status!=Status::Free||s.generation==~uint64_t(0))return false;last_ticket_=ticket;auto gen=s.generation+1;s={Status::Recording,last_seen_[slot],gen,frame,cmd,fence,ticket};return true;}
 bool Commit(uint32_t slot,uint64_t cmd,uint64_t fence,uint64_t frame,uint64_t ticket,bool ended,bool submitted){if(slot>=3)return false;auto& s=slots_[slot];if(s.status!=Status::Recording)return false;if(s.command!=cmd||s.fence!=fence||s.frame!=frame||s.ticket!=ticket||!ended||!submitted){s.status=Status::Quarantine;return false;}s.status=Status::Pending;return true;}
 bool Retire(uint32_t slot,uint64_t generation){if(slot>=3)return false;auto& s=slots_[slot];if(s.status!=Status::Ready||s.generation!=generation)return false;s.status=Status::Free;return true;}
 const Slot* Get(uint32_t slot)const{return slot<3?&slots_[slot]:nullptr;}
 uint64_t Serial(uint32_t slot)const{return slot<3?last_seen_[slot]:0;}
private:std::array<Slot,3> slots_{};std::array<uint64_t,3> last_seen_{};uint64_t last_ticket_=0;
};
struct BoundTickets {
    DeviceBinding binding;
    Tickets tickets;
    bool Release(uintptr_t address,uint64_t generation,bool idleSucceeded) {
        if(!binding.Release(address,generation,idleSucceeded))return false;
        tickets=Tickets{};
        return true;
    }
};
}
