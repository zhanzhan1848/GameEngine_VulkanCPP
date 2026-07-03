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
        // Read the metallib file into memory. Caller writes it to a tmp
        // file before passing to MTL::Device::newLibraryWithURL — see
        // Initialize() below for the rationale.
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

        // === Load via newLibraryWithURL, not newLibrary(dispatch_data_t) ===
        // Engine/Graphics/Metal/MetalShader.cpp::load_engine_shaders hit a
        // SIGBUS on macOS 26 / Apple Silicon when calling
        // newLibrary(dispatch_data_t): Metal's newLibraryWithData runs
        // CC_SHA256_Update via NEON-accelerated
        // AccelerateCrypto_SHA256_compress, and any dispatch_data_t whose
        // backing buffer is page-aligned with a non-multiple-of-64 size
        // SIGBUS-es during the 64-byte SHA256 read. The work-around that
        // file uses is to write the metallib to a tmp file and load via
        // newLibraryWithURL, which goes through a different code path with
        // proper alignment guarantees. We do the same here.
        std::vector<u8> bytes;
        if (!read_metallib_bytes(path, bytes)) {
            std::cerr << "[MetalBlitToDrawable] failed to read " << path << "\n";
            return false;
        }

        // mkstemps requires a writable char buffer; .data() on a non-const
        // std::string is fine in C++17.
        std::string tmp{ "/tmp/primal_blit_to_drawable_XXXXXX.metallib" };
        int fd = mkstemps(tmp.data(), 9);  // 9 = strlen(".metallib")
        if (fd < 0) {
            std::cerr << "[MetalBlitToDrawable] mkstemps failed\n";
            return false;
        }
        const ssize_t written = ::write(fd, bytes.data(), bytes.size());
        ::close(fd);
        if (written != static_cast<ssize_t>(bytes.size())) {
            ::unlink(tmp.c_str());
            std::cerr << "[MetalBlitToDrawable] short write to tmp file\n";
            return false;
        }

        NS::String* tmp_path = NS::String::string(tmp.c_str(), NS::UTF8StringEncoding);
        NS::URL* url = NS::URL::fileURLWithPath(tmp_path);
        NS::Error* err{ nullptr };
        MTL::Library* lib = device->newLibrary(url, &err);
        ::unlink(tmp.c_str());
        tmp_path->release();
        if (!lib || err) {
            std::cerr << "[MetalBlitToDrawable] newLibraryWithURL failed for " << path
                      << ": " << (err ? err->localizedDescription()->utf8String() : "null library") << "\n";
            if (err) err->release();
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
