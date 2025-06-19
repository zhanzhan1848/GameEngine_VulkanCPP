#include "MetalCore.h"

#include "MetalSurface.h"
#include "MetalShader.h"
#include "MetalPostProcess.h"
#include "MetalResource.h"
#include "shaders/ShaderType.h"
#include "MetalCamera.h"
#include "MetalGPass.h"
#include "MetalContent.h"

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
                _cmd_buffer->presentDrawable( pView->currentDrawable() );
                _cmd_buffer->commit();

                pView->draw();
                
                _frame_index = (_frame_index + 1) % frame_buffer_count;

                return true;
            }

            void flush()
			{
				_frame_index = 0;
			}

            void release()
            {
                flush();
                core::release(_command_queue);
                core::release(_device);
            }

            [[nodiscard]] constexpr MTL::CommandBuffer* command_buffer() const { return _cmd_buffer; }
            [[nodiscard]] constexpr u32 frame_index() const { return _frame_index; }
            
        private:
            MTL::Device*                        _device{};
            MTL::CommandQueue*                  _command_queue{};
            NS::AutoreleasePool*                _pool{};
            MTL::CommandBuffer*                 _cmd_buffer;
            u32									_frame_index{ 0 };
            dispatch_semaphore_t                _semaphore;
        };

        MTL::Device* _device{ nullptr };

        bool create_device()
        {
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

        metal_frame_info get_metal_frame_info(frame_info info, constant_buffer& cbuffer, const metal_surface& surface, u32 frame_idx, f32 delta_time)
        {
            camera::metal_camera& camera{ camera::get(info.camera_id) };
			camera.update();
			msl::GlobalShaderData data{};

			data.View = camera.view();
			data.Projection = camera.projection();
			data.InvProjection = camera.inverse_projection();
			data.ViewProjection = camera.view_projection();
			data.InvViewProjection = camera.inverse_view_projection();
			data.CameraPositionAndViewWidth = math::v4{ camera.position().x(), camera.position().y(), camera.position().z(), (f32)surface.width() };
			data.CameraDirectionAndViewHeight = math::v4{ camera.direction().x(), camera.direction().y(), camera.direction().z(), (f32)surface.height() };
			data.NumDirectionalLights = static_cast<u32>(info.light_set_key);
			data.DeltaTime = delta_time;

            // NOTE: be careful not to read from this buffer. Reads are really really slow
			msl::GlobalShaderData *const shader_data{ cbuffer.allocate<msl::GlobalShaderData>() };
			// TODO: handle the case when cbuffer is full.
			memcpy(shader_data, &data, sizeof(msl::GlobalShaderData));

			metal_frame_info metal_info
			{
				&info,
				&camera,
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
			new (&constants_buffer[i]) constant_buffer{ constant_buffer::get_default_init_info(1024 * 1024) };
			NAME_METAL_OBJECT_INDEXED(constants_buffer[i].buffer(), i, "Global Constant Buffer");
		}

        new (&gfx_command) metal_command(_device);

        if(!(shader::initialize() 
            && gpass::initialize()
            && fx::initialize()
            && content::initialize()
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
        
        content::shutdown();
        fx::shutdown();
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
    }

    MTL::Device* get_device()
    {
        return _device;
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
            get_metal_frame_info(info, cbuffer, *surface, frame_idx, 16.7f) 
        };

        gpass::set_size({ metal_info.surface_width, metal_info.surface_height });

        if (gfx_command.begin_frame())
        {
            //
            // ....
            //

            // Record commands
            MTL::CommandBuffer* cmd_buffer{ gfx_command.command_buffer() };

            // Depth prepass
            gpass::depth_prepass(cmd_buffer, metal_info);

            // Geometry pass
            gpass::render(cmd_buffer, metal_info);

            // Post process
            fx::post_process(cmd_buffer, surface, cbuffer);

            gfx_command.end_frame(surface);
        }
    }
}