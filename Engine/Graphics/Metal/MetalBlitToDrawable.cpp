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
                // 1. Standalone BlitToDrawable.metallib (preferred — set up by
                //    CMake/EngineShaderBlob.cmake when the dedicated name is
                //    emitted). The file currently just contains the same
                //    metallib blob as shaders.metallib because the source
                //    shader is bundled alongside the editor shaders.
                // The cwd is pinned to the binary's directory at startup
                // (see Program::AnchorWorkingDirectoryToExecutable), so the
                // shaders/ dir is a direct cwd-relative subdir.
                "./shaders/metal/BlitToDrawable.metallib",
                // 2. Cmake build dir (when launched from the build dir).
                "./Darwin/Debug/shaders/metal/BlitToDrawable.metallib",
                "./Darwin/Release/shaders/metal/BlitToDrawable.metallib",
                "./Darwin/PrimalEditor_Avalonia/DebugEditor/shaders/metal/BlitToDrawable.metallib",
                "./Darwin/PrimalEditor_Avalonia/ReleaseEditor/shaders/metal/BlitToDrawable.metallib",
                // 3. Engine-relative for tests that chdir into EngineTest/.
                "../Darwin/Debug/shaders/metal/BlitToDrawable.metallib",
                "../Darwin/Release/shaders/metal/BlitToDrawable.metallib",
                "../shaders/metal/BlitToDrawable.metallib",
                // 4. Fall back to the main shaders.metallib (which now also
                //    contains blit_to_drawable_vs/fs since the shader is
                //    compiled by build_engine_shader_blob.py alongside
                //    EditorShaders.metal). Loaded by symbol lookup — same
                //    library, so the function lookup works identically.
                "./shaders/metal/shaders.metallib",
                "./Darwin/Debug/shaders/metal/shaders.metallib",
                "./Darwin/Release/shaders/metal/shaders.metallib",
                "./Darwin/PrimalEditor_Avalonia/DebugEditor/shaders/metal/shaders.metallib",
                "./Darwin/PrimalEditor_Avalonia/ReleaseEditor/shaders/metal/shaders.metallib",
                "../Darwin/Debug/shaders/metal/shaders.metallib",
                "../Darwin/Release/shaders/metal/shaders.metallib",
                "../shaders/metal/shaders.metallib",
            };
            for (const char* p : candidates) {
                if (fs::exists(p)) return std::string(p);
            }
            // 4. Env var override (CI / custom install layouts).
            //    ENGINE_SHADER_ROOT should point at "<repo>/Darwin/<Config>".
            if (const char* root = std::getenv("ENGINE_SHADER_ROOT")) {
                for (const char* suffix : {
                         "/shaders/metal/BlitToDrawable.metallib",
                         "/shaders/metal/shaders.metallib",
                     }) {
                    std::string p = std::string(root) + suffix;
                    if (fs::exists(p)) return p;
                }
            }
            return std::string{};
        }

        // Read the metallib file into memory and write it to a tmp file
        // before passing to MTL::Device::newLibraryWithURL. See Initialize()
        // for the rationale (newLibrary(dispatch_data_t) SIGBUS-es on
        // macOS 26 / Apple Silicon due to a NEON SHA256 alignment bug).
        bool read_metallib_bytes(const std::string& path, std::vector<u8>& out)
        {
            std::ifstream file{ path, std::ios::binary | std::ios::ate };
            if (!file) return false;
            const auto size = file.tellg();
            if (size <= 0) return false;
            file.seekg(0, std::ios::beg);
            out.resize(static_cast<size_t>(size));
            return !!file.read(reinterpret_cast<char*>(out.data()), size);
        }
    } // anonymous namespace

    MetalBlitToDrawable& MetalBlitToDrawable::Instance()
    {
        static MetalBlitToDrawable inst;
        return inst;
    }

    bool MetalBlitToDrawable::Initialize(MTL::Device* device)
    {
        if (sampler_) return true;       // idempotent
        if (!device) return false;

        device_ = device;

        // Sampler setup is the only init-time allocation. The library is
        // loaded lazily inside Blit() on first PSO creation, so a missing
        // metallib doesn't fail the whole singleton here — surfaces that
        // don't need blitting (e.g. headless tests) still work.
        MTL::SamplerDescriptor* sdesc = MTL::SamplerDescriptor::alloc()->init();
        sdesc->setMinFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
        sdesc->setMagFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
        sdesc->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
        sdesc->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
        sampler_ = device->newSamplerState(sdesc);
        sdesc->release();

        return sampler_ != nullptr;
    }

    MTL::Library*
    MetalBlitToDrawable::load_or_get_library()
    {
        if (library_) return library_;
        if (!device_) return nullptr;

        // Locate the precompiled metallib. Built by:
        //   xcrun -sdk macosx metal    -c BlitToDrawable.metal -o BlitToDrawable.air
        //   xcrun -sdk macosx metallib    BlitToDrawable.air    -o BlitToDrawable.metallib
        const std::string path = resolve_metallib_path();
        if (path.empty()) {
            std::cerr << "[MetalBlitToDrawable] BlitToDrawable.metallib not found\n";
            return nullptr;
        }

        std::vector<u8> bytes;
        if (!read_metallib_bytes(path, bytes)) {
            std::cerr << "[MetalBlitToDrawable] failed to read " << path << "\n";
            return nullptr;
        }

        // mkstemps requires a writable char buffer; .data() on a non-const
        // std::string is fine in C++17.
        std::string tmp{ "/tmp/primal_blit_to_drawable_XXXXXX.metallib" };
        int fd = mkstemps(tmp.data(), 9);  // 9 = strlen(".metallib")
        if (fd < 0) {
            std::cerr << "[MetalBlitToDrawable] mkstemps failed\n";
            return nullptr;
        }
        const ssize_t written = ::write(fd, bytes.data(), bytes.size());
        ::close(fd);
        if (written != static_cast<ssize_t>(bytes.size())) {
            ::unlink(tmp.c_str());
            std::cerr << "[MetalBlitToDrawable] short write to tmp file\n";
            return nullptr;
        }

        // We deliberately use autoreleased factories here because:
        //  (a) newLibraryWithURL goes through newLibraryWithData, which on
        //      macOS 26 / Apple Silicon SIGBUS-es on dispatch_data_t with
        //      certain alignments — MetalShader.cpp documents the same.
        //  (b) NS::String::string(...) and NS::URL::fileURLWithPath(...) are
        //      autoreleased; calling ->release() on them here is the
        //      over-release that crashed the timer callback's per-callout
        //      autorelease pool drain. Do NOT release them.
        //  (c) NS::Error** out-parameter is NOT owned by us; do NOT release it.
        NS::String* tmp_path = NS::String::string(tmp.c_str(), NS::UTF8StringEncoding);
        NS::URL* url = NS::URL::fileURLWithPath(tmp_path);
        NS::Error* err{ nullptr };
        MTL::Library* lib = device_->newLibrary(url, &err);
        ::unlink(tmp.c_str());
        if (!lib) {
            std::cerr << "[MetalBlitToDrawable] newLibraryWithURL failed for " << path
                      << ": " << (err ? err->localizedDescription()->utf8String() : "null library") << "\n";
            return nullptr;
        }
        // lib is retained (+1 from newLibrary). Cache and return.
        library_ = lib;
        return library_;
    }

    MTL::RenderPipelineState*
    MetalBlitToDrawable::create_pipeline_for_format(MTL::PixelFormat format)
    {
        if (!device_ || !sampler_) return nullptr;

        MTL::Library* lib = load_or_get_library();
        if (!lib) return nullptr;

        // function names are autoreleased factory strings; newFunction does
        // not retain them. The autorelease pool owns them until this scope
        // ends. Do NOT release them.
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
            return nullptr;
        }

        MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vs.get());
        desc->setFragmentFunction(fs.get());
        // Match the destination drawable's pixel format exactly. Hard-coding
        // a single format here was the root cause of the embedded MTKView
        // (BGRA8Unorm) producing a black surface when the PSO was built for
        // RGBA16Float — Metal rejected the mismatch and the draw silently
        // disappeared.
        desc->colorAttachments()->object(0)->setPixelFormat(format);

        // psoErr is an NS::Error** out-parameter; Metal owns it. Do NOT
        // release it — doing so over-released an autoreleased NSError that
        // the timer callback's per-callout ARP later dereferenced, which
        // was the SIGSEGV signature `objc_release → ... → ARPEnd`.
        NS::Error* psoErr{ nullptr };
        MTL::RenderPipelineState* pso = device_->newRenderPipelineState(desc, &psoErr);
        desc->release();
        if (!pso) {
            std::cerr << "[MetalBlitToDrawable] PSO create failed for format "
                      << static_cast<int>(format);
            if (psoErr) std::cerr << ": " << psoErr->localizedDescription()->utf8String();
            std::cerr << "\n";
            return nullptr;
        }
        return pso;
    }

    void MetalBlitToDrawable::Shutdown()
    {
        for (auto& kv : pipelines_) {
            // core::release() nulls the pointer after calling ->release().
            core::release(kv.second);
        }
        pipelines_.clear();
        // library_ is retained; release once.
        core::release(library_);
        core::release(sampler_);
        device_ = nullptr;
    }

    void MetalBlitToDrawable::Blit(MTL::CommandBuffer* cmd,
                                   MTL::Texture* src,
                                   MTL::Texture* drawable_texture)
    {
        if (!sampler_ || !cmd || !src || !drawable_texture) return;

        const MTL::PixelFormat fmt = drawable_texture->pixelFormat();
        MTL::RenderPipelineState* pso = pipelines_[fmt];
        if (!pso) {
            pso = create_pipeline_for_format(fmt);
            if (!pso) {
                std::cerr << "[MetalBlitToDrawable] no PSO for drawable format "
                          << static_cast<int>(fmt) << "; skipping blit\n";
                return;
            }
            pipelines_[fmt] = pso;
        }

        // Mirror MetalPostProcess::post_process: build a render pass descriptor
        // targeting the drawable's texture. Load=DontCare / Store=Store so we
        // don't need a clear and the result lands in the drawable.
        MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::renderPassDescriptor();
        if (!rpd) return;
        rpd->colorAttachments()->object(0)->setTexture(drawable_texture);
        rpd->colorAttachments()->object(0)->setLoadAction(MTL::LoadAction::LoadActionDontCare);
        rpd->colorAttachments()->object(0)->setStoreAction(MTL::StoreAction::StoreActionStore);

        MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(rpd);
        enc->setRenderPipelineState(pso);
        enc->setFragmentTexture(src, 0);
        enc->setFragmentSamplerState(sampler_, 0);
        enc->drawPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();

        // DO NOT call rpd->release() here. rpd is autoreleased (R=1 in
        // the calling thread's pool) — its lifetime is owned by the
        // autoreleasepool. The previous code had a comment claiming
        // "+1 autoreleased object" and called release() "to balance
        // our retain", but renderPassDescriptor() does NOT return a
        // +1 retained object; it returns an autoreleased one. Calling
        // release() on it over-decremented the refcount to 0 while
        // the autoreleasepool still held a reference, producing a
        // use-after-free: the autoreleasepool later drained and
        // observed that the slot's isa pointer was 0xD2... (garbage
        // from a recycled allocation), causing objc_release to fault
        // at an unmapped VM region with the SIGSEGV signature
        // `objc_release → AutoreleasePoolPage::releaseUntil →
        // objc_autoreleasePoolPop` on the GCD worker / main thread /
        // libdispatch's last-resort pool. Verified by NSZombieEnabled
        // which printed `*** -[MTLRenderPassDescriptorInternal
        // release]: message sent to deallocated instance 0x...`
        // pointing exactly at this line. Other RenderPassDescriptor
        // sites (MetalPreProcess.cpp, MetalGPass.cpp, MetalTAA.cpp,
        // MetalPostProcess.cpp, MetalCommandBuffer.cpp) correctly
        // do NOT call release() — see them for the canonical pattern.
    }
} // namespace primal::graphics::metal