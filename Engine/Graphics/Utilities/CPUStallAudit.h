#pragma once
// Explicitly opt-in, render-thread CPU instrumentation. No GPU operations.
#include "Engine/Common/CoreDiag.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace primal::graphics::cpu_stall_audit {
using Clock=std::chrono::steady_clock;
inline bool Flag(const char* name){const char* p=std::getenv(name);return p&&std::strcmp(p,"1")==0;}
inline bool Timing(){static const bool v=Flag("PRIMAL_CPU_STALL_AUDIT");return v;}
inline bool History(){static const bool v=Flag("PRIMAL_DDGI_HISTORY_CPU_AUDIT");return v;}
struct Record {char name[96]{};double ms=0;uint32_t graph=0;};
struct HistoryRecord {
    char volume[64]{};
    uint32_t frame=0,slot=0,volume_frame=0,probe_count=0,ring_slot=~0u,ring_recorded=~0u,copies=0,updates=0,rays=0;
    uint64_t completion_serial=0,ring_recorded_native=~uint64_t(0);
    uint32_t mode=0,budget=0,gi_frame=0,delta_bits=0,variance_skip=0,variance_source_frame=~0u;
    uint64_t variance_source_native=~uint64_t(0),variance_completed_serial=0,variance_source_ticket=0;
    bool warmup=false,volume_uploaded=false,variance_fenced=false,variance_accepted=false,variance_confirmed=false;
    uint64_t max_hash=14695981039346656037ull,score_hash=14695981039346656037ull,list_hash=14695981039346656037ull,age_hash=14695981039346656037ull,seed_hash=14695981039346656037ull;
    bool ready=false,map_ok=false,list_ok=false,scores_valid=false,far=false,first=false;
};
struct State {bool active=false;uint64_t frame=0,scene_epoch=0,source_digest=0,slot_serial=0;uint32_t slot=~0u,graph=0,count=0,hcount=0,governor_scale_bits=0;bool slot_completed=false,slot_timing_available=false,governor_valid=false;bool overflow=false;Record records[1024]{};HistoryRecord history[4]{};Clock::time_point start{},build_start{};bool building=false;};
inline thread_local State state;
inline void VolumeName(HistoryRecord& h,const char* text){if(std::snprintf(h.volume,sizeof(h.volume),"%s",text)>=int(sizeof(h.volume)))state.overflow=true;for(char& c:h.volume)if(c=='"'||c=='\\'||(c && static_cast<unsigned char>(c)<32))c='_';}
inline void Hash(uint64_t& h,uint32_t v){for(unsigned k=0;k<4;++k){h^=(v>>(k*8))&255u;h*=1099511628211ull;}}
inline uint32_t SeedHash32(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
inline void Selection(HistoryRecord& h,uint32_t probe){
    Hash(h.list_hash,probe);
    if(h.volume_uploaded && h.mode==7) {
        Hash(h.seed_hash,probe);Hash(h.seed_hash,h.volume_frame);Hash(h.seed_hash,h.rays);
        for(uint32_t ray=0;ray<h.rays;++ray)Hash(h.seed_hash,SeedHash32(h.volume_frame^0x9e3779b9u)^SeedHash32(ray^0x85ebca6bu));
    }
}
inline void Add(const char* name,double ms){if(!state.active||!Timing())return;if(state.count>=1024){state.overflow=true;return;}auto& r=state.records[state.count++];if(std::snprintf(r.name,sizeof(r.name),"%s",name)>=int(sizeof(r.name)))state.overflow=true;for(char& c:r.name)if(c=='"'||c=='\\'||(c && static_cast<unsigned char>(c)<32))c='_';r.ms=ms;r.graph=state.graph;}
inline void BeginGraph(){if(state.active&&Timing()){++state.graph;state.building=false;}}
inline void BuildStart(){if(state.active&&Timing()){state.build_start=Clock::now();state.building=true;}}
inline void BuildEnd(){if(state.active&&Timing()&&state.building){Add("graph.build",std::chrono::duration<double,std::milli>(Clock::now()-state.build_start).count());state.building=false;}}
struct Scope {const char* name;Clock::time_point start;explicit Scope(const char* n):name(n),start(state.active&&Timing()?Clock::now():Clock::time_point{}){}~Scope(){if(state.active&&Timing())Add(name,std::chrono::duration<double,std::milli>(Clock::now()-start).count());}};
inline HistoryRecord* NewHistory(){if(!state.active||!History())return nullptr;if(state.hcount>=4){state.overflow=true;return nullptr;}return &state.history[state.hcount++];}
inline void BindSlot(uint32_t slot,uint64_t serial,bool completed,bool available){if(state.active){state.slot=slot;state.slot_serial=serial;state.slot_completed=completed;state.slot_timing_available=available;}}
inline void SceneWitness(uint64_t epoch,uint64_t digest){if(state.active){state.scene_epoch=epoch;state.source_digest=digest;}}
inline void GovernorScale(float scale){if(state.active){std::memcpy(&state.governor_scale_bits,&scale,sizeof(scale));state.governor_valid=true;}}
struct RunGuard {
    Clock::time_point begin{};bool enabled=false,begin_ok=false,submit_called=false,submit_ok=false,frame_complete=false;static inline thread_local Clock::time_point last_start{};static inline thread_local double last_run=0,last_emit=0;double interval=0,outside=0;
    explicit RunGuard(uint64_t frame){enabled=Timing()||History();if(!enabled)return;begin=Clock::now();interval=last_start==Clock::time_point{}?0:std::chrono::duration<double,std::milli>(begin-last_start).count();outside=last_start==Clock::time_point{}?0:interval-last_run;last_start=begin;state={};state.active=true;state.frame=frame;state.start=begin;}
    void BeginResult(bool value){if(enabled)begin_ok=value;}
    void SubmitResult(bool value){if(enabled){submit_called=true;submit_ok=value;}}
    void ReachedFrameTail(){if(enabled)frame_complete=begin_ok&&submit_called&&submit_ok;}
    void Finish(){if(!enabled)return;enabled=false;const auto end=Clock::now();const double previous=last_run;last_run=std::chrono::duration<double,std::milli>(end-begin).count();state.active=false;
        static thread_local char out[262144];size_t n=0;bool truncated=false;auto append=[&](const char* f,auto... a){if(n>=sizeof(out))return;int x=std::snprintf(out+n,sizeof(out)-n,f,a...);if(x<0||size_t(x)>=sizeof(out)-n){state.overflow=true;truncated=true;n=sizeof(out)-1;}else n+=size_t(x);};
        append("[CPU_STALL_AUDIT] {\"frame\":%llu,\"slot\":%u,\"slot_serial\":%llu,\"slot_completed\":%u,\"slot_timing_available\":%u,\"begin_ok\":%u,\"submit_called\":%u,\"submit_ok\":%u,\"frame_complete\":%u,\"scene_epoch\":%llu,\"source_digest\":%llu,\"governor_scale_bits\":%u,\"governor_valid\":%u,\"interval_ms\":%.6f,\"previous_run_ms\":%.6f,\"outside_previous_ms\":%.6f,\"run_ms\":%.6f,\"previous_emit_ms\":%.6f,\"overflow\":%s,\"hash_schema\":\"fnv1a64-le-u32-v1\",\"records\":[",(unsigned long long)state.frame,state.slot,(unsigned long long)state.slot_serial,unsigned(state.slot_completed),unsigned(state.slot_timing_available),unsigned(begin_ok),unsigned(submit_called),unsigned(submit_ok),unsigned(frame_complete),(unsigned long long)state.scene_epoch,(unsigned long long)state.source_digest,state.governor_scale_bits,unsigned(state.governor_valid),interval,previous,outside,last_run,last_emit,state.overflow?"true":"false");
        for(uint32_t i=0;i<state.count;++i){const auto& r=state.records[i];append("%s{\"graph\":%u,\"name\":\"%s\",\"ms\":%.6f}",i?",":"",r.graph,r.name,r.ms);}append("],\"history\":[");
        for(uint32_t i=0;i<state.hcount;++i){const auto& h=state.history[i];append("%s{\"volume\":\"%s\",\"probe_count\":%u,\"volume_frame\":%u,\"slot\":%u,\"ring_slot\":%u,\"ring_recorded\":%u,\"ring_recorded_native\":%llu,\"ring_expected_frame\":%u,\"variance_fenced\":%u,\"variance_accepted\":%u,\"variance_confirmed\":%u,\"variance_skip\":%u,\"variance_source_frame\":%u,\"variance_source_native\":%llu,\"variance_source_ticket\":%llu,\"variance_completed_serial\":%llu,\"copies\":%u,\"frame_slot_serial_no_copy_proof\":%llu,\"updates\":%u,\"rays_per_probe\":%u,\"mode\":%u,\"budget\":%u,\"gi_frame\":%u,\"delta_bits\":%u,\"volume_uploaded\":%u,\"copy_completion\":\"%s\",\"warmup\":%u,\"far_policy\":%u,\"first\":%u,\"ready\":%u,\"map_ok\":%u,\"list_ok\":%u,\"seed_valid\":%u,\"scores_valid\":%u,\"max_hash\":%llu,\"score_hash\":%llu,\"list_hash\":%llu,\"age_hash\":%llu,\"seed_hash\":%llu}",i?",":"",h.volume,h.probe_count,h.volume_frame,h.slot,h.ring_slot,h.ring_recorded,(unsigned long long)h.ring_recorded_native,(h.variance_fenced?~0u:(h.volume_frame>=2?h.volume_frame-2:~0u)),unsigned(h.variance_fenced),unsigned(h.variance_accepted),unsigned(h.variance_confirmed),h.variance_skip,h.variance_source_frame,(unsigned long long)h.variance_source_native,(unsigned long long)h.variance_source_ticket,(unsigned long long)h.variance_completed_serial,h.copies,(unsigned long long)h.completion_serial,h.updates,h.rays,h.mode,h.budget,h.gi_frame,h.delta_bits,unsigned(h.volume_uploaded),h.variance_accepted?"certified":"unknown",unsigned(h.warmup),unsigned(h.far),unsigned(h.first),unsigned(h.ready),unsigned(h.map_ok),unsigned(h.list_ok),unsigned(h.list_ok && h.volume_uploaded && h.mode==7),unsigned(h.scores_valid),(unsigned long long)h.max_hash,(unsigned long long)h.score_hash,(unsigned long long)h.list_hash,(unsigned long long)h.age_hash,(unsigned long long)h.seed_hash);}append("]}");
        if(truncated)std::snprintf(out,sizeof(out),"[CPU_STALL_AUDIT] {\"frame\":%llu,\"overflow\":true,\"record_truncated\":true,\"frame_complete\":%u}",
            (unsigned long long)state.frame,unsigned(frame_complete));
        const auto emitStart=Clock::now();primal::diag::Emit(primal::diag::kLevelInfo,"Render",out);last_emit=std::chrono::duration<double,std::milli>(Clock::now()-emitStart).count();
    }
    ~RunGuard(){Finish();}
};
}
