#pragma once

// audio S1（2026-10-14）：无依赖 WAV 头解析（RIFF/fmt/data chunk 扫描）。
// 用途：Player 侧在播放前给出确定性元信息（时长/格式/采样率），并作为
// "解码"层的第一块——错误输入明确失败而不是静默播放垃圾。
// 只读头部不解码样本；样本回放交给平台音频（macOS AVAudioPlayer）。

#include "Common/PrimitiveTypes.h"
#include <cstddef>

namespace primal::audio {

struct WavInfo {
    u16 format_tag;        // 1=PCM, 3=IEEE float（extensible 已解包为其有效值）
    u16 channels;
    u32 sample_rate;       // Hz
    u16 bits_per_sample;
    u16 block_align;       // channels * bits/8
    u64 data_bytes;
    u64 frame_count;       // data_bytes / block_align

    double duration_seconds() const
    {
        return block_align > 0 ? static_cast<double>(frame_count) / sample_rate : 0.0;
    }

    const char* format_name() const
    {
        switch (format_tag) {
            case 1: return "PCM";
            case 3: return "IEEE-float";
            default: return "unknown";
        }
    }
};

/// 解析 WAV 头。失败（非 RIFF/WAVE、缺 fmt/data、块对齐为 0）返回 false，
/// out 不变。支持 WAVE_FORMAT_EXTENSIBLE（0xFFFE——SubFormat GUID 前 2 字节
/// 为有效格式码）。
bool parse_wav_header(const void* data, std::size_t size, WavInfo& out);

} // namespace primal::audio
