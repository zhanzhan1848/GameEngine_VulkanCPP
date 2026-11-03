// AudioSpatial.cpp — audio S2 空间声/混音纯数学实现。

#include "Audio/AudioSpatial.h"

#include <cmath>
#include <cstring>

namespace primal::audio::spatial {

f32 distance_gain(const spatial_params& p, const f32 source[3], const f32 listener_pos[3])
{
    if (!p.valid()) return 0.f;
    const f32 dx = source[0] - listener_pos[0];
    const f32 dy = source[1] - listener_pos[1];
    const f32 dz = source[2] - listener_pos[2];
    const f32 d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d <= p.reference_distance) return p.max_gain;
    if (d >= p.max_distance) return p.min_gain;
    const f32 g = p.max_gain * p.reference_distance
        / (p.reference_distance + p.rolloff_factor * (d - p.reference_distance));
    return g < p.min_gain ? p.min_gain : (g > p.max_gain ? p.max_gain : g);
}

f32 stereo_pan(const listener& l, const f32 source[3])
{
    const f32 dx = source[0] - l.position[0];
    const f32 dz = source[2] - l.position[2];
    const f32 horizontal2 = dx * dx + dz * dz;
    if (horizontal2 < 1e-12f) return 0.f;
    // 前向（yaw）：0°=+Z；右向 = 前向绕 +Y 旋转 -90°（从上往下看顺时针）
    // （primal::math::pi 替代 M_PI：MSVC 的 <cmath> 不定义 POSIX 扩展 M_PI，
    //  常量经 CommonHeaders.h → Utilities/MathTypes.h 传递，双平台一致）
    const f32 yaw = l.yaw_deg * primal::math::pi / 180.f;
    const f32 fx = -sinf(yaw), fz = cosf(yaw);          // 前向单位向量
    const f32 rx = cosf(yaw), rz = sinf(yaw);           // 右向单位向量
    const f32 inv = 1.f / sqrtf(horizontal2);
    const f32 right = (dx * rx + dz * rz) * inv;        // 水平投影上的右分量 -1..1
    (void)fx; (void)fz;
    // 与前向点积仅用于对称性（前后同侧同声像，S2 无后置衰减）——直接用右分量
    return right < -1.f ? -1.f : (right > 1.f ? 1.f : right);
}

bool bus_mixer::set_master_gain(f32 gain)
{
    if (gain < 0.f || gain > 1.f || !std::isfinite(gain)) return false;
    master_ = gain;
    return true;
}

bool bus_mixer::set_group_gain(const char* group, f32 gain)
{
    if (group == nullptr || group[0] == '\0' || strlen(group) >= sizeof(groups_[0].name))
        return false;
    if (gain < 0.f || gain > 1.f || !std::isfinite(gain)) return false;
    for (auto& g : groups_)
    {
        if (g.used && strncmp(g.name, group, sizeof(g.name)) == 0)
        {
            g.gain = gain;
            return true;
        }
    }
    for (auto& g : groups_)
    {
        if (g.used) continue;
        g.used = true;
        g.gain = gain;
        strncpy(g.name, group, sizeof(g.name) - 1);
        g.name[sizeof(g.name) - 1] = '\0';
        return true;
    }
    return false;   // 分组表满
}

bool bus_mixer::group_gain(const char* group, f32& out) const
{
    if (group == nullptr) return false;
    for (const auto& g : groups_)
        if (g.used && strncmp(g.name, group, sizeof(g.name)) == 0)
        {
            out = g.gain;
            return true;
        }
    return false;
}

bool bus_mixer::resolve(const char* group, f32 voice_gain, f32& out) const
{
    f32 g = 0.f;
    if (!group_gain(group, g)) return false;
    f32 v = master_ * g * voice_gain;
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;
    out = v;
    return true;
}

} // namespace primal::audio::spatial
