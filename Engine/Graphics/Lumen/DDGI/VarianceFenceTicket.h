#pragma once
#include "Graphics/RHI/Core/FrameSlotCompletion.h"
#include <array>
#include <cstdint>
#include <limits>
namespace primal::graphics::lumen {
// Epoch zero means no independently known scene. A missing digest with an
// unchanged known epoch suspends reads but retains last confirmed CPU values.
enum class VarianceIdentityAction { Ready, Suspend, Invalidate };
class VarianceSceneIdentity {
public:
    VarianceIdentityAction Observe(uint64_t epoch,uint64_t digest) {
        if(!epoch) {const bool had=epoch_!=0 || digest_!=0;epoch_=digest_=0;available_=false;return had?VarianceIdentityAction::Invalidate:VarianceIdentityAction::Suspend;}
        if(epoch_ && epoch_!=epoch) {epoch_=epoch;digest_=digest;available_=digest!=0;return VarianceIdentityAction::Invalidate;}
        epoch_=epoch;
        if(!digest) {available_=false;return VarianceIdentityAction::Suspend;}
        if(digest_ && digest_!=digest){digest_=digest;available_=true;return VarianceIdentityAction::Invalidate;}
        digest_=digest;available_=true;return VarianceIdentityAction::Ready;
    }
    void Reset(){epoch_=digest_=0;available_=false;}
    bool Available()const{return available_;}
private: uint64_t epoch_=0,digest_=0;bool available_=false;
};
// CPU ticket authority for one volume. Recording is NOT submission/completion.
class VarianceFenceTicket {
public:
    static constexpr uint64_t Invalid=~uint64_t(0);
    struct Context {uint32_t slot=~uint32_t(0);uint64_t serial=0,command=Invalid,fence=Invalid,frame=0,ticket=0,epoch=0;};
    struct Entry {Context context{};uint64_t buffer=Invalid,bytes=0,source=Invalid,source_frame=0,scene_epoch=0,source_digest=0,source_generation=0;bool recorded=false,submitted=false,consumed=false;};
    void Reset(){if(epoch_!=Invalid)++epoch_;else exhausted_=true;for(auto& e:slots_)e={};current_={};active_=false;}
    bool Begin(const rhi::FrameSlotCompletion& completion,uint32_t slot,uint64_t command,uint64_t fence,uint64_t frame,uint64_t ticket) {
        // An abandoned prior recording cannot be authenticated by a later fence.
        if(active_){auto& old=slots_[current_.slot];if(old.recorded&&!old.submitted&&old.context.ticket==current_.ticket)old={};active_=false;}
        if(exhausted_||slot>=3||command==Invalid||fence==Invalid||ticket==0||ticket==Invalid||ticket<=last_ticket_||!completion.CanReuse(slot,last_serial_[slot]))return false;
        const uint64_t serial=completion.Serial(slot);last_serial_[slot]=serial;
        last_ticket_=ticket;
        current_={slot,serial,command,fence,frame,ticket,epoch_};active_=true;return true;
    }
    bool Active()const{return active_&&!exhausted_;}
    bool Matches(uint32_t slot,uint64_t command)const{return Active()&&current_.slot==slot&&current_.command==command;}
    void Reject(){if(active_){auto& e=slots_[current_.slot];if(e.recorded&&!e.submitted&&e.context.ticket==current_.ticket)e={};active_=false;}}
    uint32_t Slot()const{return Active()?current_.slot:~uint32_t(0);}
    // Single attempt per completed ticket, including Map failure.
    const Entry* Take(uint64_t buffer,uint64_t bytes){
        if(!Active())return nullptr;auto& e=slots_[current_.slot];
        if(!e.submitted||e.consumed)return nullptr;
        // Even a failed resource/provenance check consumes this completed ticket;
        // a later callback in the same frame may not reinterpret stale data.
        e.consumed=true;
        if(e.context.epoch!=epoch_||e.context.slot!=current_.slot||e.context.fence!=current_.fence||
           e.context.serial>=current_.serial||e.buffer!=buffer||e.bytes!=bytes||!bytes)return nullptr;
        return &e;
    }
    bool Record(uint64_t buffer,uint64_t bytes,uint64_t source,uint64_t sourceFrame,uint64_t sceneEpoch=0,uint64_t sourceDigest=0,uint64_t generation=0){
        if(!Active()||buffer==Invalid||source==Invalid||!bytes)return false;auto& e=slots_[current_.slot];
        // Repeated producer callbacks within one command are not a new ticket.
        if(e.recorded && e.context.ticket==current_.ticket)return false;
        e={};e.context=current_;e.buffer=buffer;e.bytes=bytes;e.source=source;e.source_frame=sourceFrame;e.scene_epoch=sceneEpoch;e.source_digest=sourceDigest;e.source_generation=generation;e.recorded=true;return true;
    }
    bool Commit(uint32_t slot,uint64_t command,uint64_t fence,uint64_t frame,uint64_t ticket,bool success){
        if(!Active())return false;
        if(current_.slot!=slot||current_.command!=command||current_.fence!=fence||current_.frame!=frame||current_.ticket!=ticket){Reject();return false;}
        auto& e=slots_[slot];const bool recorded=e.recorded&&e.context.ticket==current_.ticket&&e.context.epoch==epoch_;
        if(recorded){if(success)e.submitted=true;else e={};}active_=false;return recorded&&success;
    }
private:
    uint64_t epoch_=1,last_ticket_=0;std::array<uint64_t,3> last_serial_{};std::array<Entry,3> slots_{};Context current_{};bool active_=false,exhausted_=false;
 };
// Shared by the scheduler and CPU fixture: nonzero epoch change (even when
// digest is unknown) and wholly unknown epoch clear cross-scene CPU state.
template<class States>
VarianceIdentityAction ReconcileVarianceScene(VarianceSceneIdentity& identity,VarianceFenceTicket& tickets,
                            bool& confirmed,States& states,uint64_t epoch,uint64_t digest) {
    const bool wasValid=identity.Available();
    const auto action=identity.Observe(epoch,digest);
    if(action==VarianceIdentityAction::Invalidate || (!epoch && confirmed)) {
        tickets.Reset();confirmed=false;
        for(auto& state:states)state.max_depth_variance=0.f;
    } else if(action==VarianceIdentityAction::Suspend && wasValid)tickets.Reset();
    return action;
}
}
