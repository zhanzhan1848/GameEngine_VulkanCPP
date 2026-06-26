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
#include "MetalBlitToDrawable.h"
#include "Graphics/Renderer.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalTexture.h"

namespace primal::graphics::metal::core
{
    namespace
    {
        class metal_command
        {
        public:
            metal_command() = default;
            DISABLE_COPY_AND_MOVE(metal_command);
            explicit metal_command(MTL::Device* device) : _device{ device->retain() }
            {
                _command_queue = _device->newCommandQueue();
                _pool = NS::AutoreleasePool::alloc()->init();
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
                MTK::View* pView{ surface->view() };
                MTL::Drawable* drawable{ pView->currentDrawable() };

                // Schedule a present once the framebuffer is complete using the current drawable
                if( drawable )
                {
                    // Create a scheduled handler functor for Metal to present the drawable when the command
                    // buffer has been scheduled by the kernel.

                    drawable->retain();
                    _cmd_buffer->addScheduledHandler( [drawable]( MTL::CommandBuffer* ){
                        drawable->present();
                        drawable->release();
                    });
                }
                // _cmd_buffer->presentDrawable( pView->currentDrawable() );
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
                core::release(_command_queue);
                core::release(_device);
            }

            [[nodiscard]] constexpr MTL::CommandBuffer* command_buffer() const { return _cmd_buffer; }
            [[nodiscard]] constexpr u32 frame_index() const { return _frame_index; }
            [[nodiscard]] constexpr u32 frame_count() const { return _frame_count; }
            
        private:
            MTL::Device*                        _device{};
            MTL::CommandQueue*                  _command_queue{};
            NS::AutoreleasePool*                _pool{};
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
        constant_buffer                         constants_buffer[frame_buffer_count];

        utl::vector<MTL::Resource*>				deferred_releases[frame_buffer_count]{};
        u32										deferred_release_flag[frame_buffer_count]{};
		std::mutex								deferred_release_mutex{};

        void __attribute__((noinline)) process_deferred_release(u32 frame_idx)
		{
			std::lock_guard lock{ deferred_release_mutex };

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

        metal_frame_info get_metal_frame_info(frame_info info, constant_buffer& cbuffer, const metal_surface& surface, u32 frame_idx, u32 frame_count, f32 delta_time)
        {
            camera::metal_camera& camera{ camera::get(info.camera_id) };
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
			std::lock_guard lock{ deferred_release_mutex };
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

        if(!(shader::initialize() 
            && gpass::initialize()
            && fx::initialize()
            && prepass::initialize()
            && taa::initialize()
            && ssao::initialize()
            && ssgi::initialize()
            && content::initialize()
            && light::initialize()
        )) return false;

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

    void remove_surface(surface_id id)
    {
        _surfaces.remove(id);
    }

    void resize_surface(surface_id id, u32 width, u32 height)
    {

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
            get_metal_frame_info(info, cbuffer, *surface, frame_idx, gfx_command.frame_count(), 16.7f) 
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
            fx::post_process(cmd_buffer, surface, cbuffer);

            gfx_command.end_frame(surface);
        }
    }

    MTK::View* get_surface_view(surface_id id)
    {
        // _surfaces is in the anonymous namespace above. Out-of-range access
        // to utl::free_list asserts in debug; we treat invalid_id as null.
        if (!id::is_valid(id)) return nullptr;
        return _surfaces[id].view();
    }

    u32 blit_surface_and_present(surface_id id, u64 src_handle)
    {
        if (!id::is_valid(id)) return 0;

        MTK::View* view = _surfaces[id].view();
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
        // NOTE: we deliberately use a per-blit command queue rather than
        // gfx_command's queue. gfx_command is paired with render_surface's
        // frame semaphore + end_frame present cadence; reusing it from
        // Path B (which runs *after* PipelineRenderFrame, not via
        // surface::render) would desync the semaphore. A dedicated queue
        // keeps Path B self-contained and avoids interference with the
        // main render loop. The queue is heap-allocated once and cached
        // for the process lifetime. Guarded with std::call_once to avoid
        // the same race as the PSO init above.
        static MTL::CommandQueue* s_blit_queue{ nullptr };
        if (!s_blit_queue) {
            static std::once_flag queue_flag;
            std::call_once(queue_flag, []() {
                s_blit_queue = get_device()->newCommandQueue();
                // Retain for process lifetime; intentionally never released
                // (process exit reclaims it). Matches the pattern in
                // metal_command's constructor.
            });
            if (!s_blit_queue) return 0;
        }

        CA::MetalDrawable* drawable = view->currentDrawable();
        if (!drawable) return 0;

        // Current render pass descriptor would reconfigure the drawable's
        // load/store; we want our own descriptor targeting the drawable
        // texture with DontCare/Store. MetalBlitToDrawable::Blit builds it.
        MTL::Texture* drawableTex = drawable->texture();
        if (!drawableTex) return 0;

        MTL::CommandBuffer* cmd = s_blit_queue->commandBuffer();
        if (!cmd) return 0;

        blit.Blit(cmd, srcTexture, drawableTex);

        // Present + commit. presentDrawable retains the drawable across the
        // GPU schedule, so we don't need an explicit retain here (unlike
        // metal_command::end_frame which adds a scheduledHandler).
        cmd->presentDrawable(drawable);
        cmd->commit();

        // MTKView in manual mode (paused=true, enableSetNeedsDisplay=true,
        // see MetalSurface::create) needs draw() to advance currentDrawable
        // to the next frame's drawable after we present. Without this, a
        // subsequent BlitRenderTargetToSurface would re-blit into the same
        // (already-presented) drawable.
        view->draw();

        return 1;
    }
}
