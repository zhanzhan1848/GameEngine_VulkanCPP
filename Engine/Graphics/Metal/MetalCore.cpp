#include "MetalCore.h"

#include <mutex>  // std::call_once, std::once_flag

#include "MetalSurface.h"
#include "MetalShader.h"
#include "MetalPostProcess.h"
#include "MetalResource.h"
#include "shaders/ShaderType.h"
#include "MetalCamera.h"
#include "MetalGPass.h"
#include "MetalContent.h"
#include "MetalLight.h"
#include "MetalPreProcess.h"
#include "Platform/Platform.h"
#include "Platform/PlatformTypes.h"
#include <iostream>
#include "MetalBlitToDrawable.h"
#include "Graphics/Renderer.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalTexture.h"
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::metal::core
{
    namespace
    {
        class metal_command
        {
        // Authoritative note on autorelease pool ownership (July 2026 fix):
        //
        // This class intentionally does NOT own an NS::AutoreleasePool. The
        // previous design constructed a pool in the ctor and drained it in
        // release(), but that pool's lifetime spanned many AppKit run-loop
        // iterations and was popped at shutdown from inside AppKit's
        // windowWillClose autorelease pool scope. autorelease pools are
        // strictly LIFO — popping an outer pool while AppKit's inner pool
        // is still active corrupts the slot table, and the per-callout
        // autorelease pool drain at -[NSWindow __close] detects the
        // inconsistency and aborts via AutoreleasePoolPage::busted
        // (SIGKILL/EXC_CRASH, signature
        //  objc_autoreleasePoolPop → AutoreleasePoolPage::busted
        //   → WindowBaseImpl::Close).
        //
        // The fix is to delegate autorelease-pool ownership to the calling
        // thread. The render loop fires from CFRunLoopTimerCreate on the
        // AppKit main run loop, which manages its own per-callout pool —
        // that pool drains at the end of every run-loop iteration, which is
        // sufficient for the autoreleased commandBuffer / drawable refs
        // produced by begin_frame/end_frame. No per-class pool needed.
        //
        // Test paths that run off-AppKit threads must push their own local
        // pool around each render call (the only test entry currently is
        // TestRenderer/TestRendererMac, which calls initialize without
        // invoking render_surface, so no pool is needed there either).
        public:
            metal_command() = default;
            DISABLE_COPY_AND_MOVE(metal_command);
            explicit metal_command(MTL::Device* device) : _device{ device->retain() }
            {
                _command_queue = _device->newCommandQueue();
                _semaphore = dispatch_semaphore_create(frame_buffer_count);
            }

            ~metal_command() { release(); }

            bool begin_frame()
            {
                _cmd_buffer = _command_queue->commandBuffer();
                dispatch_semaphore_wait( _semaphore, DISPATCH_TIME_FOREVER );
                _cmd_buffer->addCompletedHandler( ^void( MTL::CommandBuffer* pCmd ){
                    dispatch_semaphore_signal( this->_semaphore );
                });
                return _cmd_buffer != nullptr;
            }

            bool end_frame(metal_surface* surface)
            {
                MTK::View* pView{ static_cast<MTK::View*>(surface->view()) };
                MTL::Drawable* drawable{ pView->currentDrawable() };

                // Use Metal's official presentDrawable: API instead of a hand-rolled
                // addScheduledHandler + drawable->present() pair.
                //
                // Apple's MTLDrawable.present() docs explicitly recommend calling
                // presentDrawable: instead — it schedules the drawable's present()
                // to fire *after* the command queue schedules this command buffer,
                // and Metal retains the drawable internally across the GPU schedule.
                //
                // The previous hand-rolled pattern captured `drawable` as a raw
                // pointer in a scheduled-handler block. If AppKit invalidated the
                // CAMetalDrawable between commit() and the block firing on the
                // com.Metal.CompletionQueueDispatch queue (e.g. NSWindow closing,
                // MTKView detached, view resized mid-flight), the block would call
                // objc_msgSend on freed memory → EXC_BAD_ACCESS at the smashed
                // isa pointer (crash signature: KERN_INVALID_ADDRESS at a non-VM
                // address inside -[_MTLCommandBuffer presentDrawable:options:]_block_invoke).
                //
                // See Engine/Graphics/Metal/MetalCore.cpp::blit_surface_and_present
                // and Engine/Graphics/RHI/Platforms/Metal/MetalSwapChain.cpp::Present
                // for the canonical presentDrawable pattern in this codebase.
                if( drawable )
                {
                    _cmd_buffer->presentDrawable( drawable );
                }
                _cmd_buffer->commit();
                // _cmd_buffer->waitUntilCompleted();

                // 在手动渲染模式下，必须调用 draw() 来更新 currentDrawable 到下一帧
                // 这确保了下一次调用 currentDrawable 时能获取到新的 drawable
                // 而不是已经 presented 的旧 drawable
                pView->draw();

                _frame_index = (_frame_index + 1) % frame_buffer_count;
                _frame_count++;

                return true;
            }

            void flush()
			{
				_frame_index = 0;
                _frame_count = 0;
			}

            void release()
            {
                flush();
                // Wait for any in-flight GPU work to finish before tearing down
                // any Metal objects. Without this, Metal's
                // com.Metal.CompletionQueueDispatch can fire a
                // presentDrawable: block AFTER the MTKView it captured has
                // been deallocated by AppKit — the block dereferences a
                // smashed isa pointer, producing EXC_BAD_ACCESS at a
                // non-VM address (crash signature: KERN_INVALID_ADDRESS at
                // 0x... inside -[_MTLCommandBuffer
                // presentDrawable:options:]_block_invoke). This is the
                // SIGSEGV we've seen during Avalonia editor teardown.
                //
                // waitUntilCompleted() blocks until the GPU finishes the
                // command buffer's scheduled handlers. On a healthy system
                // this returns within a frame; only a stuck GPU would hang
                // (which is itself a fatal error). After this returns, the
                // MTKView reference held by the in-flight present block is
                // released by Metal before our own autoreleased reference
                // goes out of scope.
                if (_cmd_buffer)
                {
                    const MTL::CommandBufferStatus status = _cmd_buffer->status();
                    if (status == MTL::CommandBufferStatus::CommandBufferStatusCommitted ||
                        status == MTL::CommandBufferStatus::CommandBufferStatusScheduled)
                    {
                        _cmd_buffer->waitUntilCompleted();
                    }
                    _cmd_buffer = nullptr;
                }

                core::release(_command_queue);
                core::release(_device);
            }

            [[nodiscard]] constexpr MTL::CommandBuffer* command_buffer() const { return _cmd_buffer; }
            [[nodiscard]] constexpr u32 frame_index() const { return _frame_index; }
            [[nodiscard]] constexpr u32 frame_count() const { return _frame_count; }
            
        private:
            MTL::Device*                        _device{};
            MTL::CommandQueue*                  _command_queue{};
            MTL::CommandBuffer*                 _cmd_buffer;
            u32									_frame_index{ 0 };
            u32									_frame_count{ 0 };
            dispatch_semaphore_t                _semaphore;
        };

        MTL::Device* _device{ nullptr };
        // === RHI 桥接（Phase 1 Sub-step 1.2.3'）===
        // 由 set_external_device() 注入；非空时 get_device() 优先返回它。
        // 与 _device 互斥使用：create_device() 检测到 _external_device 非空时跳过创建。
        MTL::Device* _external_device{ nullptr };

        bool create_device()
        {
            // 外部已注入 device，跳过自动创建
            if (_external_device) return true;

            _device = MTL::CreateSystemDefaultDevice();

            if(_device) return true;
            else return false;

        }

        using surface_collection = utl::free_list<metal_surface>;

        surface_collection                      _surfaces;
        metal_command                           gfx_command;

        // deferred_release_mutex is declared before constants_buffer so that,
        // per [basic.start.term]/3, it is destroyed AFTER the buffer array at
        // program exit. Reversing this order would let ~constant_buffer call
        // deferred_release on a destroyed mutex (EINVAL from pthread_mutex_lock).
        //
        // Heap-allocate so its lifetime exceeds the process teardown —
        // the GCD-worker fire_timer may call this after the main thread
        // has already begun static destruction.
        utl::vector<MTL::Resource*>				deferred_releases[frame_buffer_count]{};
        u32										deferred_release_flag[frame_buffer_count]{};
		std::mutex*								deferred_release_mutex_ptr{ new std::mutex() };

        constant_buffer                         constants_buffer[frame_buffer_count];

        void __attribute__((noinline)) process_deferred_release(u32 frame_idx)
 		{
 			if (deferred_release_mutex_ptr)
 				std::lock_guard lock{ *deferred_release_mutex_ptr };

			// NOTE: we clear this flag in the beginning. If we'd clear it at the end
			//		 then it might overwrite some other thread that was trying to set it.
			//		 It's fine if overwriting happens before processing the items.
			deferred_release_flag[frame_idx] = 0;

			utl::vector<MTL::Resource*>& resources{ deferred_releases[frame_idx] };
			if (!resources.empty())
			{
				for (auto& resource : resources) release(resource);
				resources.clear();
			}
		}

        metal_frame_info get_metal_frame_info(const frame_info& info, constant_buffer& cbuffer, const metal_surface& surface, u32 frame_idx, u32 frame_count, f32 delta_time)
        {
            camera::metal_camera& camera{ camera::get(info.camera) };
			msl::GlobalShaderData data{};

            data.PreviousViewProjection = camera.view_projection();
            
			camera.update();
			data.View = camera.view();
			data.Projection = camera.projection();
			data.InvProjection = camera.inverse_projection();
			data.ViewProjection = camera.view_projection();
            if(frame_count == 0)
            {
                data.PreviousViewProjection = data.ViewProjection;
            }
			data.InvViewProjection = camera.inverse_view_projection();
			data.CameraPositionAndViewWidth = math::v4{ camera.position().x, camera.position().y, camera.position().z, (f32)surface.width() };
			data.CameraDirectionAndViewHeight = math::v4{ camera.direction().x, camera.direction().y, camera.direction().z, (f32)surface.height() };
			data.NumDirectionalLights = static_cast<u32>(light::non_cullable_light_count(info.light_set_key));
			data.DeltaTime = delta_time;
            data.FrameCount = frame_count;

            // NOTE: be careful not to read from this buffer. Reads are really really slow
			msl::GlobalShaderData *const shader_data{ cbuffer.allocate<msl::GlobalShaderData>() };
			// TODO: handle the case when cbuffer is full.
			memcpy(shader_data, &data, sizeof(msl::GlobalShaderData));

			metal_frame_info metal_info
			{
				&info,
				&camera,
                info.light_probe_manager,
                cbuffer.buffer(),
				surface.width(),
				surface.height(),
				id::invalid_id,
				frame_idx,
				delta_time
			};

			return metal_info;
        }
    } // anonymous namespace

    namespace detail
	{
		void deferred_release(MTL::Resource* resource)
		{
			const u32 frame_idx{ current_frame_index() };
			if (deferred_release_mutex_ptr)
				std::lock_guard lock{ *deferred_release_mutex_ptr };
			deferred_releases[frame_idx].push_back(resource);
			set_deferred_release_flag();
		}
	} // detail namespace

    bool initialize()
    {
        if(!create_device()) return false;

        for (u32 i{ 0 }; i < frame_buffer_count; ++i)
		{
			new (&constants_buffer[i]) constant_buffer{ constant_buffer::get_default_init_info(2048 * 2048) };
			NAME_METAL_OBJECT_INDEXED(constants_buffer[i].buffer(), i, "Global Constant Buffer");
		}

        new (&gfx_command) metal_command(get_device());

        // shader::initialize is the EngineShaders blob path (legacy forward
        // renderer). If the blob is missing we still want graphics::initialize
        // to succeed so the RHI path (which has its own per-shader source-compile
        // fallback) can still run. Log a warning and continue.
        const bool shaders_ok = shader::initialize();
        if (!shaders_ok) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalCore] shader::initialize failed; continuing without legacy engine shaders." << std::endl;
        }

        // Each pass init may fail when its shader is unavailable (Avalonia editor
        // without a precompiled engine shaders blob). We treat each as a soft
        // failure so the rest of graphics::initialize still completes — the
        // individual pass simply won't run, and the editor preview can render
        // a clear-color frame.
        auto soft_init = [](const char* name, bool (*fn)()) {
            if (!fn()) {
                PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalCore] " << name << "::initialize failed; pass disabled."
                          << " (shader blob missing function or PSO descriptor invalid)"
                          << std::endl;
            }
        };
        soft_init("gpass",    gpass::initialize);
        soft_init("fx",       fx::initialize);
        soft_init("prepass",  prepass::initialize);
        soft_init("taa",      taa::initialize);
        soft_init("ssao",     ssao::initialize);
        soft_init("ssgi",     ssgi::initialize);
        soft_init("content",  content::initialize);
        soft_init("light",    light::initialize);

        return true;
    }

    void shutdown()
    {
        gfx_command.release();

        // NOTE: we don't call process_deferred_release at the end because
		//		 some resources (such as swap chains) can't be released before
		//       their depending resources are release
		for (u32 i{ 0 }; i < frame_buffer_count; ++i)
		{
			process_deferred_release(i);
		}
        
        light::shutdown();
        content::shutdown();
        prepass::shutdown();
        fx::shutdown();
        taa::shutdown();
        ssao::shutdown();
		ssgi::shutdown();
		gpass::shutdown();
		shader::shutdown();
		camera::shutdown();

		for (u32 i{ 0 }; i < frame_buffer_count; ++i)
		{
			constants_buffer[i].release();
		}

        // NOTE: some types only use deferred release for their resources during
		//		 shudown /rest/ clear. To finally release these resources we call
		//		 process_deferred_releases once more
		process_deferred_release(0);

        if(_device)  release(_device);

        // === RHI 桥接（Phase 1 Sub-step 1.2.3'）===
        // 释放我们对 _external_device 的 retain()，但 rhi::MetalDevice 自身的所有权不受影响。
        if(_external_device)
        {
            _external_device->release();
            _external_device = nullptr;
        }
    }

    MTL::Device* get_device()
    {
        // 外部注入优先；否则返回自动创建的 device
        return _external_device ? _external_device : _device;
    }

    void set_external_device(MTL::Device* device)
    {
        // 同一指针重复设置无操作
        if (_external_device == device) return;

        // 释放旧的 retain
        if (_external_device)
        {
            _external_device->release();
            _external_device = nullptr;
        }

        // 设置新的并 retain（rhi::MetalDevice 也持有，双引用计数安全）
        if (device)
        {
            _external_device = device->retain();
        }
    }

    u32 current_frame_index()
    {
        return static_cast<u32>(gfx_command.frame_index());
    }

    void set_deferred_release_flag() 
    { 
        deferred_release_flag[current_frame_index()] = 1; 
    }

    constant_buffer& cbuffer() { return constants_buffer[current_frame_index()]; }

    surface create_surface(platform::window window)
    {
        surface_id id{ _surfaces.add(window) };
        _surfaces[id].create();
        return surface{ id };
    }

    surface create_external_surface(platform::window win)
    {
        // The platform window must already be registered by the caller (via
        // platform::register_external_window). Previously this function also
        // called register_external_window itself, which double-registered the
        // host view in the platform's free list when the C ABI
        // CreateEmbeddedRenderSurface also registered it for the
        // render_surface window field — leaking one slot per embed and
        // leaving rs.window invalid on later attempts to resize.
        surface_id id{ _surfaces.add(win) };
        if (!_surfaces[id].create_external()) {
            // Tear down the just-allocated slot so we don't leak a half-
            // initialized metal_surface, and return an invalid surface so
            // the C ABI caller can detect failure. Without this, a host
            // view with 0×0 bounds or a failed MTKView allocation would
            // still report a valid id and the editor would enter its
            // render loop against a non-functional surface (the documented
            // P0 #3 / black-screen signature).
            _surfaces.remove(id);
            return surface{};
        }
        return surface{ id };
    }

    void* external_surface_view(surface_id id)
    {
        return _surfaces[id].view();
    }

    void external_surface_view_frame(surface_id id, s32 x, s32 y, u32 w, u32 h)
    {
        if (id >= _surfaces.size()) return;
        _surfaces[id].set_external_frame(x, y, w, h);
    }

    void remove_surface(surface_id id)
    {
        _surfaces.remove(id);
    }

    void resize_surface(surface_id id, u32 width, u32 height)
    {
        if (!id::is_valid(id)) return;
        if (width == 0 || height == 0) return;
        _surfaces[id].resize(width, height);
    }

    u32 surface_width(surface_id id)
    {
        return _surfaces[id].width();
    }

    u32 surface_height(surface_id id)
    {
        return _surfaces[id].height();
    }

    void render_surface(surface_id id, [[maybe_unused]] frame_info info)
    {
        const u32 frame_idx{ current_frame_index() };
        constant_buffer& cbuffer{ constants_buffer[frame_idx] };
        cbuffer.clear();

        metal_surface* surface{ &_surfaces[id] };

        const metal_frame_info metal_info
        { 
            get_metal_frame_info(info, cbuffer, *surface, frame_idx, gfx_command.frame_count(), info.last_frame_time)
        };

        gpass::set_size({ metal_info.surface_width, metal_info.surface_height });

        fx::set_size({ metal_info.surface_width, metal_info.surface_height });

        taa::set_size({ metal_info.surface_width, metal_info.surface_height });

        ssao::set_size({ metal_info.surface_width, metal_info.surface_height });

        ssgi::set_size({ metal_info.surface_width, metal_info.surface_height });

        if (gfx_command.begin_frame())
        {
            //
            // ....
            //

            // Record commands
            MTL::CommandBuffer* cmd_buffer{ gfx_command.command_buffer() };

            // Update light buffer
            light::update_light_buffers(metal_info);

            // Depth prepass
            gpass::depth_prepass(cmd_buffer, metal_info);

            // Shadow mapping
            prepass::prepass(cmd_buffer, metal_info);

            // Geometry pass
            gpass::render(cmd_buffer, metal_info);

            // Compose pass
            fx::compose_pass(cmd_buffer, metal_info);

            // TAA pass
            taa::taa_pass(cmd_buffer, metal_info);

            // SSAO pass
            ssao::ssao_pass(cmd_buffer, metal_info);
            ssao::ssao_blur(cmd_buffer, metal_info);

            // SSGI pass
            ssgi::ssgi_pass(cmd_buffer, metal_info);
            ssgi::ssgi_blur(cmd_buffer, metal_info);

            // Post process
            fx::post_process(cmd_buffer, surface, cbuffer, info.last_frame_time);

            gfx_command.end_frame(surface);
        }
    }

    MTK::View* get_surface_view(surface_id id)
    {
        // _surfaces is in the anonymous namespace above. Out-of-range access
        // to utl::free_list asserts in debug; we treat invalid_id as null.
        if (!id::is_valid(id)) return nullptr;
        return static_cast<MTK::View*>(_surfaces[id].view());
    }

    u32 blit_surface_and_present(surface_id id, u64 src_handle)
    {
        if (!id::is_valid(id)) return 0;

        MTK::View* view = static_cast<MTK::View*>(_surfaces[id].view());
        if (!view) return 0;

        // === Resolve src texture from the RHI device ===
        // rhi::ResourceHandle is a u64 alias; the C ABI passes it opaquely.
        // Resolve via graphics::get_rhi_device() → MetalDevice → MetalTexture.
        auto* rhiDevice = graphics::get_rhi_device();
        if (!rhiDevice) return 0;
        auto* metalDevice = dynamic_cast<rhi::MetalDevice*>(rhiDevice);
        if (!metalDevice) return 0;
        auto* metalTex = metalDevice->GetTexture(static_cast<rhi::ResourceHandle>(src_handle));
        if (!metalTex || !metalTex->GetNativeTexture()) return 0;
        MTL::Texture* srcTexture = metalTex->GetNativeTexture();

        // === Lazy-init the blit PSO ===
        // Code review flagged a check-then-act race: if Editor calls
        // BlitRenderTargetToSurface from multiple threads, two could both
        // observe !IsInitialized() and double-create the PSO / sampler /
        // metallib, leaking Metal objects. Guard with std::call_once so
        // Initialize runs at most once process-wide. A failed init still
        // leaves IsInitialized()==false, so the next caller can retry
        // (call_once only suppresses *entry*, not the result).
        auto& blit = MetalBlitToDrawable::Instance();
        if (!blit.IsInitialized()) {
            static std::once_flag init_flag;
            std::call_once(init_flag, [&blit]() {
                blit.Initialize(get_device());
            });
            if (!blit.IsInitialized()) return 0;
        }

        // === Acquire drawable + command buffer ===
        // PipelineRenderFrame submits the offscreen render to the RHI
        // device's graphics queue immediately before this function is called.
        // The blit must use that same queue: Metal only guarantees command
        // buffer ordering within one MTLCommandQueue.  A separate private
        // queue races the render with the texture sample below, so the
        // drawable can repeatedly receive the render target's initial black
        // contents even though the forward pass itself completed correctly.
        //
        // Reusing the graphics queue does not involve the legacy
        // gfx_command frame semaphore.  It merely places this standalone
        // command buffer after the RHI render command buffer, establishing
        // render -> blit -> present ordering without a CPU wait.
        MTL::CommandQueue* blit_queue = metalDevice->GetGraphicsQueue();
        if (!blit_queue) return 0;

        CA::MetalDrawable* drawable = view->currentDrawable();
        if (!drawable) return 0;

        // Validate the drawable has a usable texture size BEFORE encoding.
        // MTKView with paused=true / enableSetNeedsDisplay=true returns a
        // drawable immediately, but the drawable's backing CAMetalDrawable
        // may have a 0×0 texture when (a) the view hasn't been laid out yet
        // (Avalonia layout pass not complete, host NSView bounds = 0×0),
        // (b) the host NSView was detached and reattached with new bounds
        // but MTKView hasn't picked up the resize yet, or (c) the editor
        // is being torn down. Present-ing a 0×0 drawable produces a
        // CAMetalDrawable whose backing texture is invalid; the GPU then
        // schedules a presentDrawable: block that calls objc_msgSend on a
        // 0-sized texture reference at completion, which is the root
        // cause of the SIGSEGV on com.Metal.CompletionQueueDispatch
        // (crash signature: KERN_INVALID_ADDRESS at a non-VM address
        // inside -[_MTLCommandBuffer presentDrawable:options:]_block_invoke).
        // Bail out cleanly so the next frame can retry with valid bounds.
        MTL::Texture* drawableTex = drawable->texture();
        if (!drawableTex) return 0;
        const NS::UInteger texWidth = drawableTex->width();
        const NS::UInteger texHeight = drawableTex->height();
        std::fprintf(stderr, "[blit_surface_and_present] drawableTex=%p size=%zux%zu\n",
            (void*)drawableTex, (size_t)texWidth, (size_t)texHeight);
        std::fflush(stderr);
        if (texWidth == 0 || texHeight == 0) return 0;

        MTL::CommandBuffer* cmd = blit_queue->commandBuffer();
        if (!cmd) return 0;
        std::fprintf(stderr, "[blit_surface_and_present] cmd=%p srcTex=%p drawableTex=%p\n",
            (void*)cmd, (void*)srcTexture, (void*)drawableTex);
        std::fflush(stderr);

        blit.Blit(cmd, srcTexture, drawableTex);

        // === Explicit drawable lifetime management ===
        //
        // `currentDrawable()` returns an autoreleased CAMetalDrawable. The
        // previous code relied on the autorelease pool of this calling
        // thread staying alive until GPU completion — which is fragile:
        //
        //   - When called from Avalonia's UI thread (the editor's
        //     BlitRenderTargetToSurface path), the per-callout pool drains
        //     as soon as AppKit returns control to the run loop. The
        //     drawable's autorelease ref-count hits 0 BEFORE Metal's GPU
        //     completion queue fires the presentDrawable: completion block.
        //   - When called from a libdispatch worker (the GCD fire_timer
        //     path used by some editor tests), the worker's last-resort
        //     pool is even more aggressive about recycling slots.
        //
        // The documented fix is: explicit retain on acquire, explicit
        // release in the command buffer's completedHandler (which Metal
        // guarantees runs on the GPU completion queue, in its own
        // autorelease pool, after Metal itself is done with the drawable).
        //
        // This decouples drawable lifetime from this calling thread's
        // autorelease pool entirely. The drawable is now owned by the
        // command buffer's completion handler for the duration of GPU
        // execution, then released.
        drawable->retain();

        cmd->presentDrawable(drawable);

        __block CA::MetalDrawable* block_drawable = drawable;
        cmd->addCompletedHandler(^(MTL::CommandBuffer* /*cb*/) {
            // GPU is done with the drawable. Metal's presentDrawable:
            // completion block has fired (or been skipped on error paths —
            // addCompletedHandler fires regardless of presentDrawable:
            // success/failure). Safe to release our explicit retain.
            block_drawable->release();
        });

        cmd->commit();
        std::fprintf(stderr, "[blit_surface_and_present] presentDrawable + commit done\n");
        std::fflush(stderr);

        // MTKView in manual mode (paused=true, enableSetNeedsDisplay=true,
        // see MetalSurface::create) needs draw() to advance currentDrawable
        // to the next frame's drawable after we present. Without this, a
        // subsequent BlitRenderTargetToSurface would re-blit into the same
        // (already-presented) drawable.
        view->draw();
        std::fprintf(stderr, "[blit_surface_and_present] view->draw() done\n");
        std::fflush(stderr);

        return 1;
    }
}
