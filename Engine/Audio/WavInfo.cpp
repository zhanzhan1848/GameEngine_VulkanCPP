#include "WavInfo.h"

#include <cstring>

namespace primal::audio {
namespace {

constexpr u16 kFormatPcm = 1;
constexpr u16 kFormatIeeeFloat = 3;
constexpr u16 kFormatExtensible = 0xFFFE;

u16 read_u16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 read_u32(const u8* p)
{
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8)
         | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}
bool tag_is(const u8* p, const char (&tag)[5])
{
    return std::memcmp(p, tag, 4) == 0;
}

} // namespace

bool parse_wav_header(const void* data, std::size_t size, WavInfo& out)
{
    if (!data || size < 12) return false;
    const u8* base = static_cast<const u8*>(data);

    if (!tag_is(base, "RIFF") || !tag_is(base + 8, "WAVE")) return false;

    u16 format_tag = 0, channels = 0, bits = 0, block_align = 0;
    u32 sample_rate = 0;
    u64 data_bytes = 0;
    bool have_fmt = false, have_data = false;

    // RIFF chunk 遍历：id(4) + size(4LE) + payload（字对齐：奇数 size 补 1）。
    // 头部读取只带前 4KB：data chunk 的声明 size 是元信息（无需样本在场），
    // 截断时按声明值登记时长；其余截断 chunk（罕见 trailing 元数据）直接停。
    std::size_t offset = 12;
    while (offset + 8 <= size)
    {
        const u8* chunk = base + offset;
        const u32 chunk_size = read_u32(chunk + 4);
        const std::size_t payload = offset + 8;
        const bool truncated = payload + chunk_size > size;

        if (tag_is(chunk, "fmt ") && !truncated && chunk_size >= 16)
        {
            format_tag = read_u16(chunk + 8);
            channels = read_u16(chunk + 10);
            sample_rate = read_u32(chunk + 12);
            block_align = read_u16(chunk + 20);
            bits = read_u16(chunk + 22);
            if (format_tag == kFormatExtensible && chunk_size >= 40)
            {
                // fmt 扩展（payload 相对 fmt 段起点）：base 16B + cbSize(2) +
                // validBits(2) + channelMask(4) = 24 → SubFormat GUID 前 2 字节。
                format_tag = read_u16(chunk + 8 + 16 + 2 + 2 + 4);
            }
            have_fmt = true;
        }
        else if (tag_is(chunk, "data"))
        {
            data_bytes = chunk_size;   // 声明值（截断读取不影响时长元信息）
            have_data = true;
        }

        if (truncated) break;
        offset = payload + chunk_size + (chunk_size & 1);  // RIFF 偶对齐
    }

    if (!have_fmt || !have_data) return false;
    if (channels == 0 || sample_rate == 0 || block_align == 0) return false;
    if (format_tag != kFormatPcm && format_tag != kFormatIeeeFloat) return false;

    out.format_tag = format_tag;
    out.channels = channels;
    out.sample_rate = sample_rate;
    out.bits_per_sample = bits;
    out.block_align = block_align;
    out.data_bytes = data_bytes;
    out.frame_count = data_bytes / block_align;
    return true;
}

} // namespace primal::audio
