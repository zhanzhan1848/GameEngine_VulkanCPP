// audio S1（2026-10-14）：WAV 头解析的 C ABI 门面。
// 解码实现在 Engine/Audio/WavInfo（无依赖纯函数）；本文件只做 POD 镜像 +
// 导出——宿主（PrimalPlayer/编辑器/测试）不依赖引擎头文件（ABI 纪律：
// 导出只增不改，POD 用 static_assert 锁尺寸，沿 AssetAPI/EngineEventsAPI 惯例）。

#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Audio/WavInfo.h"
#include "Audio/AudioSpatial.h"

#include <cstddef>

// WavInfo 的 ABI 稳定镜像（字段语义见 Engine/Audio/WavInfo.h；单位：
// sample_rate=Hz，duration 由调用方按 frame_count/sample_rate 计算）。
// 布局契约：4×u16 → u32 → 2×u64（PrimalPlayer 的 PlayerWavInfo 同布局；
// 两端 static_assert 锁尺寸，字段顺序变更 = ABI 破坏，禁止）。
struct AudioWavInfoPod
{
    u16 format_tag;        // 1=PCM 3=IEEE-float
    u16 channels;
    u16 bits_per_sample;
    u16 block_align;       // channels * bits/8
    u32 sample_rate;       // Hz
    u64 data_bytes;
    u64 frame_count;
};
static_assert(sizeof(AudioWavInfoPod) == 32, "AudioWavInfoPod 布局锁（u16*4+u32+u64*2，自然对齐）");

extern "C" {

// 解析 WAV 头。失败（非 RIFF/WAVE/缺 fmt·data/块对齐 0）返回 0，out 不变。
EDITOR_INTERFACE u32 AudioParseWavHeader(const void* data, u64 size, AudioWavInfoPod* out)
{
    if (!data || !out || size == 0) return 0;
    primal::audio::WavInfo info{};
    if (!primal::audio::parse_wav_header(data, static_cast<std::size_t>(size), info))
        return 0;
    out->format_tag = info.format_tag;
    out->channels = info.channels;
    out->sample_rate = info.sample_rate;
    out->bits_per_sample = info.bits_per_sample;
    out->block_align = info.block_align;
    out->data_bytes = info.data_bytes;
    out->frame_count = info.frame_count;
    return 1;
}


// ---- audio S2（2026-10-15）：空间声 / 分组混音 / 脚本事件播放 -----------------
//
// 空间与混音是引擎纯数学（Engine/Audio/AudioSpatial，C++ 单测覆盖）；播放
// 始终在宿主（S2：宿主回调——Player 注册 AVAudioPlayer 桥；编辑器可注册预听）。
// 事件触发（Lua audio.play）经 AudioSetHostPlay 注入的回调投递——引擎不直接
// 持有音频设备。

namespace {
primal::audio::spatial::bus_mixer g_mixer;

// 宿主播放回调（进程内唯一；nullptr = 未注册——audio.play 返回失败）
using host_play_fn = u32 (*)(const char* path, f32 gain, f32 pan, u32 loop);
host_play_fn g_host_play = nullptr;
} // anonymous namespace

// 空间求值：监听者 + 声源位置 + 参数 →（gain, pan）。参数 5 浮数
// [reference_distance, rolloff, max_distance, max_gain, min_gain]；空指针=缺省。
// 返回 0 = 参数非法（gain/pan 不写）。
EDITOR_INTERFACE u32 AudioComputeSpatial(const f32* listener_pos_xyz, f32 listener_yaw_deg,
                                         const f32* source_pos_xyz, const f32* params5,
                                         f32* out_gain, f32* out_pan)
{
    if (!listener_pos_xyz || !source_pos_xyz || !out_gain || !out_pan) return 0;
    primal::audio::spatial::spatial_params p{};
    if (params5)
    {
        p.reference_distance = params5[0];
        p.rolloff_factor = params5[1];
        p.max_distance = params5[2];
        p.max_gain = params5[3];
        p.min_gain = params5[4];
    }
    if (!p.valid()) return 0;
    primal::audio::spatial::listener l{};
    l.position[0] = listener_pos_xyz[0];
    l.position[1] = listener_pos_xyz[1];
    l.position[2] = listener_pos_xyz[2];
    l.yaw_deg = listener_yaw_deg;
    *out_gain = primal::audio::spatial::distance_gain(p, source_pos_xyz, l.position);
    *out_pan = primal::audio::spatial::stereo_pan(l, source_pos_xyz);
    return 1;
}

// 分组混音：master → 分组 → voice 级联。未知分组返回 0（配置拼错要暴露）。
EDITOR_INTERFACE u32 AudioSetMasterGain(f32 gain) { return g_mixer.set_master_gain(gain) ? 1u : 0u; }
EDITOR_INTERFACE u32 AudioSetGroupGain(const char* group, f32 gain)
{
    return g_mixer.set_group_gain(group, gain) ? 1u : 0u;
}
EDITOR_INTERFACE u32 AudioResolveGain(const char* group, f32 voice_gain, f32* out)
{
    if (!out) return 0;
    return g_mixer.resolve(group, voice_gain, *out) ? 1u : 0u;
}

// 宿主播放回调注册（S2 事件触发播放的桥）。回调签名：path + 增益/声像 +
// loop；返回非 0 = 宿主已受理。传 nullptr 清除注册。
EDITOR_INTERFACE void AudioSetHostPlay(void* callback)
{
    g_host_play = reinterpret_cast<host_play_fn>(callback);
}

// 事件触发播放（Lua audio 表的落点；宿主也可直调）。voice_gain 会过分组
// 级联（group 空/null = 只过 master 钳制）。返回 0 = 未注册宿主/分组未知/宿主拒绝。
EDITOR_INTERFACE u32 AudioPlay(const char* path, const char* group, f32 voice_gain, f32 pan, u32 loop)
{
    if (!path || !path[0] || !g_host_play) return 0;
    f32 gain = voice_gain;
    if (group != nullptr && group[0] != 0)
    {
        if (!g_mixer.resolve(group, voice_gain, gain)) return 0;
    }
    else
    {
        gain = gain < 0.f ? 0.f : (gain > 1.f ? 1.f : gain);
    }
    return g_host_play(path, gain, pan, loop);
}

} // extern "C"
