#pragma once

// MetalBlitToDrawable
//
// Renders a fullscreen triangle that samples `src` into a destination
// texture (typically an MTKView's currentDrawable). Required because
// MTKView drawables are framebufferOnly (per memory: Material Preview
// Fragment Blit) — MTLBlitCommandEncoder cannot write to them, so we
// must blit via a fragment shader.
//
// Mirrors the pattern in MetalPostProcess::post_process (which already
// does the same drawable-blit for the engine's main render path), but
// packaged as a reusable helper for surface::blit_and_present.
//
// The PSO is keyed on the destination drawable's pixel format because the
// embedded Avalonia path uses MTKView BGRA8Unorm while the standalone
// window path uses RGBA16Float; a single hard-coded PSO silently fails
// validation against the wrong drawable and produces a black surface.
//
// The MTL::Library is loaded once and retained for the singleton's
// lifetime. Re-loading the metallib per PSO would create a fresh
// autoreleased NS::String tmp path + NS::URL + NS::Library every time,
// multiplying the chance of an over-release landmine in the timer's
// per-callout autorelease pool.
//
// Lifetime: singleton (Instance()), lazily initializes the sampler on
// first Blit() call from metal_surface::blit_and_present. Shutdown()
// releases every cached PSO, the cached library, and the shared sampler.
// The MTL::Device is borrowed (not owned) from metal::core::get_device().

#include "MetalCommonHeaders.h"

#include <unordered_map>

namespace primal::graphics::metal
{
    class MetalBlitToDrawable
    {
    public:
        static MetalBlitToDrawable& Instance();

        // Lazily create the sampler. Idempotent. `device` is borrowed (not
        // retained) from core::get_device(). The library is loaded lazily
        // inside Blit() on first PSO creation. Returns true once the
        // sampler is usable.
        bool Initialize(MTL::Device* device);

        // Release every cached PSO, the library, the sampler, and the
        // cached device handle. Safe to call multiple times.
        void Shutdown();

        // Record a fullscreen-triangle draw into `drawable_texture` sampling
        // from `src`. Both textures must outlive the command buffer. Caller
        // owns the command buffer (created/committed by the caller, not here).
        // `drawable_texture` is typically CA::MetalDrawable::texture().
        // Lazily creates a PSO matching `drawable_texture->pixelFormat()` if
        // one isn't already cached; logs and returns silently on failure.
        void Blit(MTL::CommandBuffer* cmd,
                  MTL::Texture* src,
                  MTL::Texture* drawable_texture);

        [[nodiscard]] bool IsInitialized() const { return sampler_ != nullptr && device_ != nullptr; }

    private:
        MetalBlitToDrawable() = default;
        ~MetalBlitToDrawable() { Shutdown(); }
        DISABLE_COPY_AND_MOVE(MetalBlitToDrawable);

        // Create a PSO whose color attachment[0] matches `format`. Returns
        // nullptr on failure (caller logs + caches nothing). Requires the
        // library to have been loaded by load_or_get_library() first.
        MTL::RenderPipelineState* create_pipeline_for_format(MTL::PixelFormat format);

        // Load the BlitToDrawable.metallib from disk and retain an
        // MTL::Library for the singleton's lifetime. Called once on first
        // PSO creation; subsequent calls return the cached library_.
        MTL::Library* load_or_get_library();

        // PSO cache keyed on the destination drawable's pixel format. Each
        // entry is retained (+1 from newRenderPipelineState); Shutdown
        // releases every entry.
        std::unordered_map<MTL::PixelFormat, MTL::RenderPipelineState*> pipelines_{};
        MTL::SamplerState*        sampler_{ nullptr };
        // Retained (owned) MTL::Library, lazily loaded. Released in
        // Shutdown(). Loading it once avoids per-frame autoreleased
        // NS::String tmp paths + NS::URL + NS::Library triples inside the
        // timer callback's per-callout autorelease pool.
        MTL::Library*             library_{ nullptr };
        // Borrowed, not retained. Caller (metal::core) owns the device.
        MTL::Device*              device_{ nullptr };
    };
} // namespace primal::graphics::metal