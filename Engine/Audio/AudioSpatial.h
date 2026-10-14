#pragma once

// AudioSpatial.h — audio S2 首切片（2026-10-15）：空间声（距离衰减 + 立体声
// 声像）与分组混音的纯数学层。无平台依赖、无状态——监听者/声源由调用方
// 传入；Player 每帧对活动声源求值并写平台播放器（AVAudioPlayer 的
// volume/pan）。C++ 单测直接覆盖。
//
// 单位：米/度，右手系 +Y 向上（与引擎一致）。

#include "CommonHeaders.h"

namespace primal::audio::spatial {

// ---- 空间参数（声源侧，S2 固定曲线族；自定义曲线是 S3 适配接口）----
struct spatial_params
{
    f32 reference_distance{ 1.f };   // 衰减参考距离（>0）
    f32 rolloff_factor{ 1.f };       // 0=无衰减；越大衰减越快
    f32 max_distance{ 500.f };       // 超过此距离不再衰减（clamp 下限）
    f32 max_gain{ 1.f };             // 距离 ≤ 参考距离时的增益上限
    f32 min_gain{ 0.f };             // 最远距离的下限增益

    bool valid() const
    {
        return reference_distance > 0.f && rolloff_factor >= 0.f
            && max_distance >= reference_distance && min_gain >= 0.f && max_gain >= min_gain;
    }
};

// ---- 监听者（S2 单监听者；位置 + 水平朝向角）----
struct listener
{
    f32 position[3]{ 0.f, 0.f, 0.f };
    /// <summary>朝向角（度）：0 = +Z，绕 +Y 顺时针（从上往下看）。
    /// 声像以此为前向基准（右手法则：+X 为右）。</summary>
    f32 yaw_deg{ 0.f };
};

/// <summary>
/// 距离衰减（clamped inverse）：d ≤ ref → max_gain；
/// d ≥ max → min_gain；之间 = max_gain × ref/(ref + rolloff×(d−ref)) 再
/// clamp 到 [min,max]。返回值即总增益（0..1）。
/// </summary>
f32 distance_gain(const spatial_params& p, const f32 source[3], const f32 listener_pos[3]);

/// <summary>
/// 立体声声像（-1 全左 .. +1 全右）。约定：前向 = yaw 的 +Z 起算向量，
/// 右向 = up×forward（右手系；yaw=0 时前 +Z、右 +X）——声源在该右向半球
/// → 正。纯水平（忽略 Y 差）；重合/正前/正后 → 0（居中；S2 无前后置区别）。
/// </summary>
f32 stereo_pan(const listener& l, const f32 source[3]);

// ---- 分组混音 ----

/// <summary>
/// 总线混音器（audio S2）：master → 命名分组 → voice 三级增益级联。
/// 固定容量分组表（确定性，无动态分配）；未注册分组读写返回 false（不静默
/// 当 1.0——配置拼错要暴露）。
/// </summary>
class bus_mixer
{
public:
    static constexpr u32 max_groups = 16;

    bool set_master_gain(f32 gain);
    f32 master_gain() const { return master_; }

    /// <summary>注册/更新分组增益（0..1）。名称 ≤ 31 字符。</summary>
    bool set_group_gain(const char* group, f32 gain);
    bool group_gain(const char* group, f32& out) const;

    /// <summary>voice 最终增益 = clamp(master × group × voice, 0, 1)。
    /// voiceGain 越界仍 clamp 计算（播放安全），分组缺失返回 false。</summary>
    bool resolve(const char* group, f32 voice_gain, f32& out) const;

private:
    struct group
    {
        char name[32]{};
        f32 gain{ 1.f };
        bool used{ false };
    };
    f32 master_{ 1.f };
    group groups_[max_groups]{};
};

} // namespace primal::audio::spatial
