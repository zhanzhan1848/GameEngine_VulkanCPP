#pragma once
#include <cstdint>
#include <cstring>

namespace primal::graphics::workgroup_experiment {
struct Receipt {
    uint32_t requested=0, actual=0, local_x=0, local_y=0, group_x=0, group_y=0;
    uint32_t width=0, height=0, slot=0;
    uint64_t scene_epoch=0, source_digest=0;
    bool dispatched=false;
    uint64_t buffers[4]{};
    bool source_qualified=false;
    uint64_t shader_fnv1a64=0;
};
inline bool Parse(const char* value, bool gather, uint32_t& out) {
    out=0;
    if (!value || std::strcmp(value,"0")==0) return true;
    if (std::strcmp(value,"32")==0) {out=32;return true;}
    if (gather && std::strcmp(value,"64")==0) {out=64;return true;}
    return false;
}
inline uint32_t Groups(uint32_t count,uint32_t local) { return count/local+(count%local!=0); }
}
