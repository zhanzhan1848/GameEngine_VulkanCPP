#include "MetalBlitToDrawable.h"

#include "MetalCore.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <filesystem>
#include <cstdlib>

#include <dispatch/dispatch.h>

namespace primal::graphics::metal
{
    namespace
    {
        // Resolve the BlitToDrawable.metallib path. The engine's metal shader
        // blob lives at "<repo>/Darwin/<Config>/shaders/metal/shaders.metallib"
        // (see engine_shader_paths[] in Renderer.cpp). We sit our metallib next
        // to that blob so the editor/test binary's cwd doesn't matter.
        //
        // We try cwd-relative paths first (matches how the engine finds its
        // own blob via get_engine_shaders_path), then the EngineTest-relative
        // path for tests that chdir into EngineTest/. If ENGINE_SHADER_ROOT is
        // set, it overrides as a last-resort override (useful for CI / custom
        // install layouts). Returns empty string if nothing found.
        std::string resolve_metallib_path()
        {
            namespace fs = std::filesystem;
            std::vector<const char*> candidates = {
                // 1. Same dir as the engine shaders blob (preferred).
                "./Darwin/Debug/shaders/metal/BlitToDrawable.metallib",
                "./Darwin/Release/shaders/metal/BlitToDrawable.metallib",
                // 2. Engine-relative for tests that chdir into EngineTest/.
                "../Darwin/Debug/shaders/metal/BlitToDrawable.metallib",
                "../Darwin/Release/shaders/metal/BlitToDrawable.metallib",
            };
            for (const char* p : candidates) {
                if (fs::exists(p)) return std::string(p);
            }
            // 3. Env var override (CI / custom install layouts).
            //    ENGINE_SHADER_ROOT should point at "<repo>/Darwin/<Config>".
            if (const char* root = std::getenv("ENGINE_SHADER_ROOT")) {
                std::string p = std::string(root) + "/shaders/metal/BlitToDrawable.metallib";
                if (fs::exists(p)) return p;
            }
            return std::string{};
        }

        // Read the metallib file into memory and wrap in a dispatch_data_t so
        // we can use device->newLibrary(dispatch_data_t, ...) — the same API
        // the engine's MetalShader.cpp uses for the shaders.metallib blob.
        // Returns nullptr on failure. Caller owns the returned dispatch_data_t
        // and must dispatch_release() it.
        dispatch_data_t read_metallib_data(const std::string& path)
        {
            std::ifstream file{ path, std::ios::binary | std::ios::ate };
            if (!file) return nullptr;
            const auto size = file.tellg();
            if (size <= 0) return nullptr;
            file.seekg(0, std::ios::beg);
            std::vector<u8> bytes(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;

            // dispatch_data_create copies (or retains the buffer). The destructor
            // ^{ } runs when the dispatch_data_t is released — we pass a nullptr
            // queue so it uses the default. The vector goes out of scope after
            // this call; dispatch_data_create with DISPATCH_DATA_DESTRUCTOR_DEFAULT
            // (nullptr destructor below is fine because the buffer is copied).
            return dispatch_data_create(
                bytes.data(),
                bytes.size(),
                nullptr,
                ^{ /* buffer is a copy, nothing to free */ });
        }
    } // anonymous namespace

    MetalBlitToDrawable& MetalBlitToDrawable::Instance()
    {
        static MetalBlitToDrawable inst;
        return inst;
    }

    bool MetalBlitToDrawable::Initialize(MTL::Device* device)
    {
        if (pipeline_) return true;       // idempotent
        if (!device) return false;

        device_ = device;

        // Locate the precompiled metallib. Built by:
        //   xcrun -sdk macosx metal    -c BlitToDrawable.metal -o BlitToDrawable.air
        //   xcrun -sdk macosx metallib    BlitToDrawable.air    -o BlitToDrawable.metallib
        const std::string path = resolve_metallib_path();
        if (path.empty()) {
            std::cerr << "[MetalBlitToDrawable] BlitToDrawable.metallib not found\n";
            return false;
        }

        dispatch_data_t data = read_metallib_data(path);
        if (!data) {
            std::cerr << "[MetalBlitToDrawable] failed to read " << path << "\n";
            return false;
        }

        NS::Error* err{ nullptr };
        MTL::Library* lib = device->newLibrary(data, &err);
        dispatch_release(data);
        if (!lib || err) {
            std::cerr << "[MetalBlitToDrawable] newLibrary failed for " << path << "\n";
            if (err) { std::cerr << "  metal error: " << err->localizedDescription()->utf8String() << "\n"; err->release(); }
            if (lib) lib->release();
            return false;
        }

        // MTL::Library::newFunction takes NS::String* (matches MetalShader.cpp usage).
        auto release_ns = [](NS::Object* o) { if (o) o->release(); };
        std::unique_ptr<MTL::Function, decltype(release_ns)> vs{
            lib->newFunction(NS::String::string("blit_to_drawable_vs", NS::UTF8StringEncoding)),
            release_ns
        };
        std::unique_ptr<MTL::Function, decltype(release_ns)> fs{
            lib->newFunction(NS::String::string("blit_to_drawable_fs", NS::UTF8StringEncoding)),
            release_ns
        };
        if (!vs || !fs) {
            std::cerr << "[MetalBlitToDrawable] function lookup failed\n";
            lib->release();
            return false;
        }

        MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vs.get());
        desc->setFragmentFunction(fs.get());
        // MTKView in MetalSurface::create() sets RGBA16Float. Match it. The
        // render pass descriptor we hand to Blit() always targets the
        // drawable's texture, whose pixel format matches the view.
        desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA16Float);

        NS::Error* psoErr{ nullptr };
        pipeline_ = device->newRenderPipelineState(desc, &psoErr);
        desc->release();
        if (!pipeline_ || psoErr) {
            std::cerr << "[MetalBlitToDrawable] PSO create failed\n";
            if (psoErr) { std::cerr << "  metal error: " << psoErr->localizedDescription()->utf8String() << "\n"; psoErr->release(); }
            lib->release();
            return false;
        }

        MTL::SamplerDescriptor* sdesc = MTL::SamplerDescriptor::alloc()->init();
        sdesc->setMinFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
        sdesc->setMagFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
        sdesc->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
        sdesc->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
        sampler_ = device->newSamplerState(sdesc);
        sdesc->release();

        lib->release();
        return true;
    }

    void MetalBlitToDrawable::Shutdown()
    {
        // core::release() nulls the pointer after calling ->release().
        core::release(pipeline_);
        core::release(sampler_);
        device_ = nullptr;
    }

    void MetalBlitToDrawable::Blit(MTL::CommandBuffer* cmd,
                                   MTL::Texture* src,
                                   MTL::Texture* drawable_texture)
    {
        if (!pipeline_ || !cmd || !src || !drawable_texture) return;

        // Mirror MetalPostProcess::post_process: build a render pass descriptor
        // targeting the drawable's texture. Load=DontCare / Store=Store so we
        // don't need a clear and the result lands in the drawable.
        MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::renderPassDescriptor();
        if (!rpd) return;
        rpd->colorAttachments()->object(0)->setTexture(drawable_texture);
        rpd->colorAttachments()->object(0)->setLoadAction(MTL::LoadAction::LoadActionDontCare);
        rpd->colorAttachments()->object(0)->setStoreAction(MTL::StoreAction::StoreActionStore);

        MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(rpd);
        enc->setRenderPipelineState(pipeline_);
        enc->setFragmentTexture(src, 0);
        enc->setFragmentSamplerState(sampler_, 0);
        enc->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();

        // renderPassDescriptor() returns a +1 autoreleased object; release our retain.
        rpd->release();
    }
} // namespace primal::graphics::metal
