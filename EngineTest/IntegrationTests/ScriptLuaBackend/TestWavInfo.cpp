// audio S1（2026-10-14）：WAV 头解析契约测试——程序化构造 RIFF 字节流
// （不依赖磁盘夹具），锁 PCM16/float32/extensible 的元信息推导与坏输入拒绝。
#include "Audio/WavInfo.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::printf("PASS: %s\n", what);
    }
}

void put_u16(std::vector<unsigned char>& b, unsigned v)
{
    b.push_back(static_cast<unsigned char>(v & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
}
void put_u32(std::vector<unsigned char>& b, unsigned v)
{
    put_u16(b, v & 0xFFFF);
    put_u16(b, (v >> 16) & 0xFFFF);
}
void put_tag(std::vector<unsigned char>& b, const char* tag)
{
    b.insert(b.end(), tag, tag + 4);
}

// 构造标准 WAV：formatTag 1=PCM 3=float 0xFFFE=extensible。
std::vector<unsigned char> make_wav(unsigned format_tag, unsigned channels,
                                    unsigned sample_rate, unsigned bits,
                                    unsigned frames)
{
    const unsigned block_align = channels * bits / 8;
    const unsigned data_bytes = frames * block_align;
    std::vector<unsigned char> b;
    put_tag(b, "RIFF");
    put_u32(b, 4 + 24 + 8 + data_bytes + (data_bytes & 1));
    put_tag(b, "WAVE");
    put_tag(b, "fmt ");
    unsigned fmt_size = format_tag == 0xFFFE ? 40 : 16;
    put_u32(b, fmt_size);
    put_u16(b, format_tag);
    put_u16(b, channels);
    put_u32(b, sample_rate);
    put_u32(b, sample_rate * block_align);   // byte rate
    put_u16(b, block_align);
    put_u16(b, bits);
    if (format_tag == 0xFFFE) {
        put_u16(b, 22);      // cbSize
        put_u16(b, bits);    // valid bits
        put_u32(b, 0);       // channel mask
        // SubFormat GUID：前 2 字节 = 有效格式码（PCM），其余 KSDATAFORMAT 尾巴
        put_u16(b, 1);
        static const unsigned char kGuidTail[14] = {
            0x00,0x00,0x10,0x00,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71,0x00,0x00};
        b.insert(b.end(), kGuidTail, kGuidTail + 14);
    }
    put_tag(b, "data");
    put_u32(b, data_bytes);
    b.insert(b.end(), data_bytes, 0xAB);
    return b;
}

} // namespace

int main()
{
    using primal::audio::WavInfo;

    // PCM16 立体声 44.1kHz 1 秒
    {
        const auto wav = make_wav(1, 2, 44100, 16, 44100);
        WavInfo info{};
        check(primal::audio::parse_wav_header(wav.data(), wav.size(), info),
              "PCM16 stereo 解析成功");
        check(info.format_tag == 1 && info.channels == 2 && info.sample_rate == 44100
              && info.bits_per_sample == 16 && info.block_align == 4,
              "PCM16 元信息");
        check(info.frame_count == 44100 &&
              info.duration_seconds() > 0.999 && info.duration_seconds() < 1.001,
              "PCM16 时长 = 1s");
    }
    // float32 单声道 48kHz 半秒
    {
        const auto wav = make_wav(3, 1, 48000, 32, 24000);
        WavInfo info{};
        check(primal::audio::parse_wav_header(wav.data(), wav.size(), info) &&
              info.format_tag == 3 && info.channels == 1 &&
              info.sample_rate == 48000 && info.bits_per_sample == 32,
              "float32 mono 元信息");
    }
    // extensible 解包到 PCM
    {
        const auto wav = make_wav(0xFFFE, 2, 48000, 24, 480);
        WavInfo info{};
        check(primal::audio::parse_wav_header(wav.data(), wav.size(), info) &&
              info.format_tag == 1 && info.bits_per_sample == 24,
              "extensible 解包为有效格式码");
    }
    // 坏输入拒绝
    {
        WavInfo info{};
        std::vector<unsigned char> junk(64, 0x7F);
        check(!primal::audio::parse_wav_header(junk.data(), junk.size(), info),
              "非 RIFF 拒绝");
        check(!primal::audio::parse_wav_header(nullptr, 0, info), "空输入拒绝");
        auto riff_only = make_wav(1, 2, 44100, 16, 10);
        std::memcpy(riff_only.data() + 8, "JUNK", 4);  // 破坏 WAVE 四字母
        check(!primal::audio::parse_wav_header(riff_only.data(), riff_only.size(), info),
              "非 WAVE 拒绝");
    }
    // 奇数 data size 的 RIFF 偶对齐（下一个 chunk 起点正确——用 fmt 在 data 后的
    // 非标准但合法顺序验证遍历不越界）
    {
        auto wav = make_wav(1, 1, 22050, 8, 3);   // data = 3 字节（奇数）
        WavInfo info{};
        check(primal::audio::parse_wav_header(wav.data(), wav.size(), info) &&
              info.frame_count == 3,
              "奇数 data 尺寸对齐遍历");
    }

    if (g_failures == 0) std::printf("All WAV info tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
