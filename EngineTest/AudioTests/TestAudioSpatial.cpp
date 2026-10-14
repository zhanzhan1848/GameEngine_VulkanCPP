// TestAudioSpatial.cpp — audio S2 空间声/混音契约测试（纯数学，无设备依赖）。
//
// 距离衰减：参考距离内=max、单调下降、max 距离钳到 min、rolloff=0 恒满、
// 非法参数 0；声像：正右=+1、正左=-1、正前=0、重合=0、yaw 旋转语义；
// 混音：三级级联+clamp、未知分组 false、满表拒绝、非法增益拒绝。

#include "CommonHeaders.h"
#include "Audio/AudioSpatial.h"

#include <cmath>
#include <cstring>
#include <iostream>

using namespace primal;
namespace sp = primal::audio::spatial;

namespace
{
int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestAudioSpatial] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestAudioSpatial] PASS: " << name << std::endl;
    }
}

void test_distance_gain()
{
    sp::spatial_params p{};
    const f32 L[3] = { 0, 0, 0 };

    check(sp::distance_gain(p, L, L) == 1.f, "dist: 参考距离内 = max_gain");

    const f32 near_[3] = { 0.5f, 0, 0 };
    const f32 mid[3] = { 2.f, 0, 0 };
    const f32 far_[3] = { 4.f, 0, 0 };
    check(sp::distance_gain(p, near_, L) == 1.f, "dist: 0.5m 仍满");
    const f32 g2 = sp::distance_gain(p, mid, L);
    const f32 g4 = sp::distance_gain(p, far_, L);
    check(g2 < 1.f && g4 < g2, "dist: 单调下降");
    // inverse 公式：d=2 → 1/(1+1×1)=0.5；d=4 → 1/(1+3)=0.25
    check(std::fabs(g2 - 0.5f) < 1e-6f, "dist: inverse 公式 0.5");
    check(std::fabs(g4 - 0.25f) < 1e-6f, "dist: inverse 公式 0.25");

    const f32 beyond[3] = { 1000.f, 0, 0 };
    check(sp::distance_gain(p, beyond, L) == 0.f, "dist: max 距离钳到 min_gain");

    sp::spatial_params flat = p;
    flat.rolloff_factor = 0.f;
    const f32 any_d[3] = { 100.f, 0, 0 };
    check(sp::distance_gain(flat, any_d, L) == 1.f, "dist: rolloff=0 恒满");

    sp::spatial_params bad = p;
    bad.reference_distance = -1.f;
    check(sp::distance_gain(bad, mid, L) == 0.f, "dist: 非法参数 → 0（不装）");

    sp::spatial_params clamped = p;
    clamped.min_gain = 0.4f;
    check(sp::distance_gain(clamped, far_, L) >= 0.4f, "dist: 下限钳生效");
}

void test_stereo_pan()
{
    sp::listener l{};   // pos 0, yaw 0（前向 +Z）
    const f32 front[3] = { 0, 0, 5 };
    const f32 right[3] = { 5, 0, 0 };
    const f32 left[3] = { -5, 0, 0 };
    const f32 center[3] = { 0, 0, 0 };
    check(std::fabs(sp::stereo_pan(l, front)) < 1e-6f, "pan: 正前 = 0");
    check(sp::stereo_pan(l, right) > 0.99f, "pan: 正右 = +1");
    check(sp::stereo_pan(l, left) < -0.99f, "pan: 正左 = -1");
    check(std::fabs(sp::stereo_pan(l, center)) < 1e-6f, "pan: 重合 = 0");
    // 高度差不影响声像（纯水平）
    const f32 above[3] = { 0, 50, 5 };
    check(std::fabs(sp::stereo_pan(l, above)) < 1e-6f, "pan: 垂直差被忽略");
    // 约定 right = up×forward：yaw=90 → 前向 -X、右向 +Z → 原"正前"(+Z) 在右
    sp::listener turned{};
    turned.yaw_deg = 90.f;
    check(sp::stereo_pan(turned, front) > 0.99f, "pan: yaw=90 后原前方在右");
    sp::listener turned_back{};
    turned_back.yaw_deg = 180.f;    // 前向 -Z → 原正前在正后，仍居中
    check(std::fabs(sp::stereo_pan(turned_back, front)) < 1e-6f, "pan: 正后 = 0");
}

void test_bus_mixer()
{
    sp::bus_mixer m;
    check(m.set_master_gain(0.8f), "mix: master 设置");
    check(m.set_group_gain("sfx", 0.5f), "mix: sfx 分组");
    check(m.set_group_gain("music", 1.f), "mix: music 分组");

    f32 out = 0.f;
    check(m.resolve("sfx", 0.5f, out) && std::fabs(out - 0.2f) < 1e-6f,
          "mix: 三级级联 0.8×0.5×0.5=0.2");
    check(m.resolve("music", 1.5f, out) && out == 1.f, "mix: voice 越界钳 1");
    check(!m.resolve("missing", 1.f, out), "mix: 未知分组 false（不静默）");
    check(!m.set_master_gain(1.5f) && !m.set_group_gain("sfx", -0.1f),
          "mix: 非法增益拒绝");
    f32 g = 0.f;
    check(m.group_gain("sfx", g) && g == 0.5f, "mix: 分组读取");
    // 更新覆盖
    check(m.set_group_gain("sfx", 0.9f) && m.group_gain("sfx", g) && g == 0.9f,
          "mix: 分组更新覆盖");
    // 满表
    sp::bus_mixer full;
    char name[8] = "g0";
    bool all_ok = true;
    for (int i = 0; i < sp::bus_mixer::max_groups; ++i)
    {
        name[1] = static_cast<char>('0' + i % 10);
        name[2] = static_cast<char>('a' + i / 10);
        name[3] = '\0';
        all_ok = full.set_group_gain(name, 1.f) && all_ok;
    }
    check(all_ok && !full.set_group_gain("overflow", 1.f), "mix: 满表第 17 分组拒绝");
}

} // anonymous namespace

int main()
{
    std::cout << "[TestAudioSpatial] === begin ===" << std::endl;
    test_distance_gain();
    test_stereo_pan();
    test_bus_mixer();
    std::cout << "[TestAudioSpatial] === end, failures=" << failures << " ===" << std::endl;
    return failures ? 1 : 0;
}
