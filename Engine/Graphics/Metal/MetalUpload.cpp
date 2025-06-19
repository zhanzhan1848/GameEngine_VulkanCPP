#include "MetalUpload.h"

#include "MetalCore.h"

namespace primal::graphics::metal::upload
{
    namespace
    {
        struct upload_frame
        {
            MTL::CommandBuffer*			cmd_buffer{ nullptr };
            MTL::Buffer*				upload_buffer{ nullptr };
            void*						cpu_address{ nullptr };
            u64                         fence_value{ 0 };

            void wait_and_reset();

            void release()
            {
                wait_and_reset();
                core::release(upload_buffer);
            }

            constexpr bool is_ready() const { return upload_buffer == nullptr; }
        };

        MTL::Device*                             device;
        constexpr u32                            upload_frame_count{ 4 }; //
        upload_frame                             upload_frames[upload_frame_count]{};
        MTL::CommandQueue*                       upload_cmd_queue{ nullptr };
        MTL::SharedEvent*                        upload_event{ nullptr };
        u64                                      upload_event_value{ 0 };
        dispatch_semaphore_t                     fence_event{};
        std::mutex                               frame_mutex{};
        std::mutex                               queue_mutex{};

        void upload_frame::wait_and_reset()
        {
            assert(upload_event && fence_event);
            // TODO: different from D3D12 upload context, maybe will change this
            //       such as compare with fence_value and sharedevent completed value
            if (upload_event->signaledValue() < fence_value)
            {
                dispatch_semaphore_wait( fence_event, DISPATCH_TIME_FOREVER );
                cmd_buffer->addCompletedHandler( ^void( MTL::CommandBuffer* cmd_buffer ){
                    dispatch_semaphore_signal( fence_event );
                });
                cmd_buffer->commit();
                cmd_buffer->waitUntilCompleted(); // CPU 等待 GPU 完成
            }

            core::release(upload_buffer);
            cpu_address = nullptr;
        }

        // NOTE: frames should be locked before this function is called.
		u32 get_available_upload_frame()
        {
            u32 index{ u32_invalid_id };
            const u32 count{ upload_frame_count };
            upload_frame *const frames{ &upload_frames[0] };
            for (u32 i{ 0 }; i < count; ++i)
            {
                if (frames[i].is_ready())
                {
                    index = i;
                    break;
                }
            }

            // None of the frames were done uploading. We're the only thread here, so
			// we can iterate through frames until we find one that is ready
            if(index == u32_invalid_id)
            {
                index = 0;
                while(!frames[index].is_ready())
                {
                    index = (index + 1) % count;
                    std::this_thread::yield();
                }
            }

            return index;
        }

        bool init_failed()
		{
			shutdown();
			return false;
		}
    } // anonymous namespace

    metal_upload_context::metal_upload_context(u32 aligned_size)
    {
        assert(upload_cmd_queue);
        {
			// We don't want to lock this function for longer than necessary. So, we scope this lock.
			std::lock_guard lock{ frame_mutex };
			_frame_index = get_available_upload_frame();
			assert(_frame_index != u32_invalid_id);
			// Before unlocking, we prevent other threads from picking
			// this frame by masking is_ready return false.
			upload_frames[_frame_index].upload_buffer = (MTL::Buffer*)1;
		}

        upload_frame& frame{ upload_frames[_frame_index] };
        frame.upload_buffer = device->newBuffer(aligned_size, MTL::ResourceStorageModeManaged);
        NAME_METAL_OBJECT_INDEXED(frame.upload_buffer, aligned_size, "Upload Buffer - size");

        frame.cpu_address = frame.upload_buffer->contents();
        assert(frame.cpu_address);
        
        _cmd_buffer = frame.cmd_buffer;
        _upload_buffer = frame.upload_buffer;
        _cpu_address = frame.cpu_address;
        assert(_cmd_buffer && _upload_buffer && _cpu_address);
    }

    void metal_upload_context::end_upload()
    {
        assert(_frame_index != u32_invalid_id);
        upload_frame& frame{ upload_frames[_frame_index] };
        MTL::CommandBuffer *const cmd_buffer{ frame.cmd_buffer };
        MTL::CommandEncoder* encoder{ cmd_buffer->blitCommandEncoder() };
        assert(encoder);
        encoder->endEncoding();

        std::lock_guard lock{ queue_mutex };
        ++upload_event_value;
        frame.fence_value = upload_event_value;
        cmd_buffer->encodeSignalEvent(upload_event, upload_event_value);
        
        frame.wait_and_reset();
        DEBUG_OP(new (this) metal_upload_context{});
    }

    bool initialize()
    {
        device = core::get_device();
        assert(device && !upload_cmd_queue);
        upload_cmd_queue = device->newCommandQueue();
        if(!upload_cmd_queue)
        {
            return init_failed();
        }
        NAME_METAL_OBJECT(upload_cmd_queue, "Upload Command Queue");
        for(u32 i{0}; i < upload_event_value; ++i)
        {
            upload_frame& frame{ upload_frames[i] };
            frame.cmd_buffer = upload_cmd_queue->commandBuffer();
            if(frame.cmd_buffer) return init_failed();
        }
        upload_event = device->newSharedEvent();
        assert(upload_event);
        if(!upload_event) return init_failed();
        fence_event = dispatch_semaphore_create(0);
        assert(fence_event);
        if(!fence_event) return init_failed();

        return true;
    }

    void shutdown()
    {
        for(u32 i{0}; i < upload_frame_count; ++i)
        {
            upload_frames[i].release();
        }

        if(fence_event)
        {
            fence_event = nullptr;
        }

        core::release(upload_cmd_queue);
        core::release(upload_event);
        upload_event_value = 0;
    }
}