/**
 * @file ImageCompare.h
 * @brief PNG save/load + Wang-Bovik SSIM for cross-backend image comparison.
 *
 * Used by Phase 4b Tier 2 (Vulkan ForwardRenderer parity test) to compare
 * a rendered frame against a Metal reference PNG. Single TU
 * (ImageCompare.cpp) owns STB_IMAGE_WRITE_IMPLEMENTATION +
 * STB_IMAGE_IMPLEMENTATION to avoid ODR violations across test binaries.
 *
 * All functions are header-only declarations; link against ImageCompare.cpp
 * (or include in the same target's sources).
 */

#pragma once

#include <cstdint>
#include <vector>

namespace EngineTest {

/// Save an RGBA8 buffer (4 bytes/pixel, row-major top-to-bottom) to a PNG file.
/// Returns true on success.
bool SavePNG(const char* path, const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h);

/// Load a PNG file into an RGBA8 buffer (4 bytes/pixel).
/// Output is resized to w*h*4. Returns true on success.
bool LoadPNG(const char* path, std::vector<std::uint8_t>& out, std::uint32_t& w, std::uint32_t& h);

/// Convert an RGBA16F (half-float, 8 bytes/pixel, little-endian) buffer to
/// RGBA8 (4 bytes/pixel) by tone-mapping each channel to [0, 1] then to [0, 255].
/// `src` length must be w*h*8; `dst` length must be w*h*4.
/// Uses Reinhard-style tonemap: c / (1 + c), then sRGB curve, then 255.
void RGBA16FToRGBA8(const std::uint8_t* src, std::uint8_t* dst,
                    std::uint32_t w, std::uint32_t h);

/// Flip an RGBA8 buffer in place along the horizontal midline (row 0 ↔ row h-1).
/// Used to normalize Vulkan's framebuffer Y axis (NDC +Y → image bottom) to
/// match Metal/OpenGL convention (NDC +Y → image top) for cross-backend SSIM.
/// `rgba` length must be w*h*4; row stride = w*4.
void FlipYInPlace(std::uint8_t* rgba, std::uint32_t w, std::uint32_t h);

/// Convert a D32_FLOAT depth buffer (4 bytes/pixel, [0, 1] range) to RGBA8
/// grayscale (RGB = depth * 255, A = 255) for visual inspection and SSIM.
/// `src` length must be w*h*4; `dst` length must be w*h*4.
/// Values outside [0, 1] are clamped; NaN/Inf become 0.
void Depth32ToRGBA8(const std::uint8_t* src, std::uint8_t* dst,
                    std::uint32_t w, std::uint32_t h);

/// Wang-Bovik SSIM (Structural Similarity Index) between two RGBA8 buffers.
/// Computes per-channel mean SSIM on luminance (Rec.709), averaged over an
/// 8×8 sliding window with Gaussian weighting (sigma=1.5).
/// Returns a float in [-1, 1]; identical images return 1.0.
/// Implementation follows Wang, Bovik, Sheikh, Simoncelli (2004).
float ComputeSSIM(const std::uint8_t* a, const std::uint8_t* b,
                  std::uint32_t w, std::uint32_t h);

/// Maximum absolute per-channel difference between two RGBA8 buffers,
/// in [0, 255]. Deterministic assertion primitive complementary to SSIM
/// (P4c-F1 onwards: exact roundtrip checks use this, not SSIM).
int MaxAbsDiff(const std::uint8_t* a, const std::uint8_t* b,
               std::uint32_t w, std::uint32_t h);

/// Convert a D16_UNORM depth buffer (2 bytes/pixel, [0, 1] range) to RGBA8
/// grayscale (RGB = depth * 255, A = 255), mirroring Depth32ToRGBA8.
/// `src` length must be w*h*2; `dst` length must be w*h*4.
void Depth16ToRGBA8(const std::uint8_t* src, std::uint8_t* dst,
                    std::uint32_t w, std::uint32_t h);

} // namespace EngineTest
