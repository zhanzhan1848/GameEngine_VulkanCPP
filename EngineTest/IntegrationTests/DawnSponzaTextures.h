#pragma once

#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "DawnSponzaAlbedoMips.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace primal::tests::dawn_sponza {

// Albedo bytes are sRGB encoded; alpha, normal and ORM components are linear.
// Albedo mips use native WGSL linear blits, matching the frozen Vulkan path.
// Keep this upload path shared by the Sponza host and the native GPU test.
inline graphics::rhi::ResourceHandle CreateTexture(
    graphics::rhi::DawnDevice& device, int width, int height,
    const unsigned char* pixels, graphics::rhi::DataFormat format) {
    using namespace graphics::rhi;
    if (!pixels || width <= 0 || height <= 0 ||
        (format != DataFormat::RGBA8_UNorm && format != DataFormat::RGBA8_sRGB))
        return handles::INVALID_RESOURCE;

    TextureDesc desc{};
    desc.size = {u32(width), u32(height), 1};
    desc.format = format;
    desc.type = TextureType::Texture2D;
    desc.mipLevels = 1;
    for (int dim = std::max(width, height); dim > 1; dim /= 2) ++desc.mipLevels;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | TextureUsage::CopySource;
    if (format == DataFormat::RGBA8_sRGB) desc.usage = desc.usage | TextureUsage::RenderTarget;
    const auto texture = device.CreateTexture(desc);
    if (texture == handles::INVALID_RESOURCE) return texture;
    device.UpdateTextureData(texture, pixels, 0, 0, 0, width, height, 1, u32(width) * 4, 0);

    if (format == DataFormat::RGBA8_sRGB) {
        if (!GenerateAlbedoMips(device, texture)) {
            device.DestroyTexture(texture);
            return handles::INVALID_RESOURCE;
        }
        return texture;
    }

    const unsigned char* source = pixels;
    std::vector<unsigned char> previous;
    for (u32 mip = 1; mip < desc.mipLevels; ++mip) {
        const int nextWidth = std::max(1, width / 2);
        const int nextHeight = std::max(1, height / 2);
        std::vector<unsigned char> next(size_t(nextWidth) * nextHeight * 4);
        // Area box filter includes the last row/column for odd dimensions.
        for (int y = 0; y < nextHeight; ++y) {
            const double y0 = double(y) * height / nextHeight;
            const double y1 = double(y + 1) * height / nextHeight;
            for (int x = 0; x < nextWidth; ++x) {
                const double x0 = double(x) * width / nextWidth;
                const double x1 = double(x + 1) * width / nextWidth;
                double sums[4]{};
                for (int sy = int(y0); sy < int(std::ceil(y1)); ++sy) {
                    const double wy = std::min(y1, double(sy + 1)) - std::max(y0, double(sy));
                    for (int sx = int(x0); sx < int(std::ceil(x1)); ++sx) {
                        const double weight = wy * (std::min(x1, double(sx + 1)) - std::max(x0, double(sx)));
                        for (int c = 0; c < 4; ++c) {
                            const auto value = source[(size_t(sy) * width + sx) * 4 + c];
                            sums[c] += weight * (double(value) / 255.0);
                        }
                    }
                }
                for (int c = 0; c < 4; ++c) {
                    double value = sums[c] / ((x1 - x0) * (y1 - y0));
                    next[(size_t(y) * nextWidth + x) * 4 + c] =
                        static_cast<unsigned char>(std::clamp(std::lround(value * 255.0), 0l, 255l));
                }
            }
        }
        device.UpdateTextureData(texture, next.data(), 0, 0, 0, nextWidth, nextHeight, 1, u32(nextWidth) * 4, mip);
        previous = std::move(next);
        source = previous.data();
        width = nextWidth;
        height = nextHeight;
    }
    return texture;
}

} // namespace primal::tests::dawn_sponza
