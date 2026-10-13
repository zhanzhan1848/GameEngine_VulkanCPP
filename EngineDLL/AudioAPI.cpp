// audio S1（2026-10-14）：WAV 头解析的 C ABI 门面。
// 解码实现在 Engine/Audio/WavInfo（无依赖纯函数）；本文件只做 POD 镜像 +
// 导出——宿主（PrimalPlayer/编辑器/测试）不依赖引擎头文件（ABI 纪律：
// 导出只增不改，POD 用 static_assert 锁尺寸，沿 AssetAPI/EngineEventsAPI 惯例）。

#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Audio/WavInfo.h"

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

} // extern "C"
