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
// Lifetime: singleton (Instance()), lazy-initialized on first Blit() call
// from metal_surface::blit_and_present. Shutdown() releases the PSO.
// The MTL::Device is borrowed (not owned) from metal::core::get_device().

#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
    class MetalBlitToDrawable
    {
    public:
        static MetalBlitToDrawable& Instance();

        // Lazily create the PSO + sampler. Idempotent. `device` is borrowed
        // (not retained) from core::get_device(). Returns true if the PSO is
        // usable after the call (including the "already initialized" case).
        bool Initialize(MTL::Device* device);

        // Release PSO + sampler. Safe to call multiple times.
        void Shutdown();

        // Record a fullscreen-triangle draw into `drawable_texture` sampling
        // from `src`. Both textures must outlive the command buffer. Caller
        // owns the command buffer (created/committed by the caller, not here).
        // `drawable_texture` is typically CA::MetalDrawable::texture().
        void Blit(MTL::CommandBuffer* cmd,
                  MTL::Texture* src,
                  MTL::Texture* drawable_texture);

        [[nodiscard]] bool IsInitialized() const { return pipeline_ != nullptr; }

    private:
        MetalBlitToDrawable() = default;
        ~MetalBlitToDrawable() { Shutdown(); }
        DISABLE_COPY_AND_MOVE(MetalBlitToDrawable);

        MTL::RenderPipelineState* pipeline_{ nullptr };
        MTL::SamplerState*        sampler_{ nullptr };
        // Borrowed, not retained. Caller (metal::core) owns the device.
        MTL::Device*              device_{ nullptr };
    };
} // namespace primal::graphics::metal
