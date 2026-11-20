#include "EngineTest/IntegrationTests/DawnSponzaProbeSchedule.h"
#include <cstdio>
#include <cstdlib>
#include <set>
#ifdef DAWN_PROBE_SCHEDULE_FROZEN_ORACLE
#include DAWN_PROBE_SCHEDULE_FROZEN_ORACLE
#endif
using primal::tests::dawn_sponza::ProbeSchedule;
static void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "ProbeSchedule: %s\n", message); std::exit(1); }
}
int main() {
    using S = ProbeSchedule;
    const S::Grid grid{{14,7,9},{-20.105f,-2.485f,-12.39f},3.f};
    S schedule; S::Plan p; S::DepthHistory empty;
    Require(!schedule.Initialize({{0,7,9},{},3.f}), "empty grid rejected");
    Require(schedule.Initialize(grid), "initialize");
    Require(!schedule.Prepare(1,1,1,882,90,{},empty,p), "skipped frame rejected");
    Require(schedule.Prepare(0,0,0,882,90,{},empty,p) && p.indices.empty(), "first warmup no dispatch");
    Require(!schedule.Commit(1), "wrong commit identity");
    schedule.Cancel();
    Require(schedule.Prepare(0,0,0,882,0,{},empty,p) && p.indices.size()==882 && p.indices.back()==881, "warmup zero first sequential list");
    schedule.Cancel();
    Require(schedule.Prepare(0,0,0,882,90,{},empty,p) && schedule.Commit(0), "frame zero commit");
    Require(schedule.Prepare(1,1,1,882,90,{},empty,p) && p.indices.empty() && schedule.Commit(1), "frame one warmup");
    Require(schedule.Prepare(2,2,2,220,90,{},empty,p) && p.indices.size()==220, "frozen priority restores budget during warmup");
    const auto prior = p.indices; schedule.Cancel();
    Require(schedule.Prepare(2,2,2,220,90,{},empty,p) && p.indices==prior && schedule.Commit(2), "cancel preserves selection and age");
    std::vector<float> depth(882*128);
    Require(!schedule.Prepare(3,3,3,220,90,{}, {depth,1,false},p), "uncompleted readback rejected");
    Require(!schedule.Prepare(3,3,3,220,90,{}, {depth,2,true},p), "wrong N-2 source rejected");
    depth[64]=std::numeric_limits<float>::quiet_NaN();
    Require(!schedule.Prepare(3,3,3,220,90,{}, {depth,1,true},p), "nonfinite variance rejected");
    depth[64]=0;
    Require(schedule.Prepare(3,3,3,220,90,{}, {depth,1,true},p) && schedule.Commit(3), "rejected inputs do not advance state");
    for (std::uint32_t frame=4;frame<400;++frame) {
        Require(schedule.Prepare(frame,frame,frame,220,90,{}, {depth,frame-2,true},p), "steady state schedule");
        Require(p.indices.size()==220 && std::set(p.indices.begin(),p.indices.end()).size()==220, "unique bounded updates");
        Require(schedule.Commit(frame), "commit steady state");
    }
    for (auto frame : schedule.LastUpdates()) Require(frame>380, "uncapped age prevents starvation");
    // High variance must affect selection even when the camera stays fixed.
    S near, varied; Require(near.Initialize({{2,1,1},{},3}) && varied.Initialize({{2,1,1},{},3}), "two probe fixture");
    for (std::uint32_t frame=0;frame<3;++frame) {
        Require(near.Prepare(frame,frame,frame,2,90,{},empty,p)&&near.Commit(frame), "near priming");
        Require(varied.Prepare(frame,frame,frame,2,90,{},empty,p)&&varied.Commit(frame), "variance priming");
    }
    std::vector<float> pair(256); const std::array<float,3> camera{0,10,0};
    Require(near.Prepare(3,3,3,1,90,camera,{pair,1,true},p)&&p.indices[0]==0, "distance priority");
    pair[128+127]=9;
    Require(varied.Prepare(3,3,3,1,90,camera,{pair,1,true},p)&&p.indices[0]==1, "all 64 variance bins considered");
#ifdef DAWN_PROBE_SCHEDULE_FROZEN_ORACLE
    RunFrozenOracle();
#endif
    std::printf("Dawn probe scheduling: admission, temporal identity, cancellation, variance and starvation checks passed\n");
}
