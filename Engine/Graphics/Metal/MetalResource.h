#pragma once

#ifndef _METAL_RESOURCE_H_
#define _METAL_RESOURCE_H_

#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
    struct metal_buffer_init_info
    {
		MTL::Heap* heap{ nullptr };
        const void* data{ nullptr };
        u32 size{ 0 };
        u32 alignment{ 0 };
    };

    class metal_buffer
    {
    public:
        metal_buffer() = default;
        explicit metal_buffer(metal_buffer_init_info info, bool is_cpu_accessible);

        DISABLE_COPY(metal_buffer);
        constexpr metal_buffer(metal_buffer&& other) noexcept
            : _buffer(other._buffer)
            , _size(other._size)
        {
            other.reset();
        }

        constexpr metal_buffer& operator=(metal_buffer&& other) noexcept
        {
            assert(this != &other);
			if (this != &other)
			{
				release();
				move(other);
			}
			return *this;
        }

        ~metal_buffer() { release(); }

        void release();

        [[nodiscard]] MTL::Buffer* buffer() const { return _buffer; }
        [[nodiscard]] constexpr u32 size() const { return _size; }
        [[nodiscard]] constexpr bool is_valid() const { return _buffer != nullptr; }
        [[nodiscard]] constexpr bool is_empty() const { return _buffer == nullptr; }

    private:
        constexpr void move(metal_buffer& other)
		{
			_buffer = other._buffer;
			_size = other._size;
			other.reset();
		}

		constexpr void reset()
		{
			if (_buffer)
			{
				_buffer->release();
				_buffer = nullptr;
			}
			_size = 0;
		}

        MTL::Buffer*        _buffer{ nullptr };
        u32                 _size{ 0 };
    };

    class constant_buffer
	{
	public:
		constant_buffer() = default;
		explicit constant_buffer(metal_buffer_init_info info);
		DISABLE_COPY_AND_MOVE(constant_buffer);
		~constant_buffer() { release(); }

		void release()
		{
			_buffer.release();
			_cpu_address = nullptr;
			_cpu_offset = 0;
		}

		constexpr void clear() { _cpu_offset = 0; }
		[[nodiscard]] u8 *const allocate(u32 size);

		template<typename T>
		[[nodiscard]] T *const allocate()
		{
			return (T *const)allocate(sizeof(T));
		}

		[[nodiscard]] constexpr MTL::Buffer *const buffer() const { return _buffer.buffer(); }
		[[nodiscard]] constexpr u32 size() const { return _buffer.size(); }
		[[nodiscard]] constexpr u8 *const cpu_address() const { return _cpu_address; }
		[[nodiscard]] constexpr u32 cpu_offset() const { return _cpu_offset; }

		template<typename T>
		[[nodiscard]] u64 offset(T *const allocation) {
			std::lock_guard lock{_mutex};
			assert(_cpu_address && allocation);
			if (!_cpu_address) return 0;
			const u8* address = reinterpret_cast<const u8*>(allocation);
			assert(address >= _cpu_address && address <= _cpu_address + _cpu_offset);
			return (u64)(address - _cpu_address);
		}

		[[nodiscard]] constexpr static metal_buffer_init_info get_default_init_info(u32 size)
		{
			assert(size);
			metal_buffer_init_info info{};
			info.size = size;
			info.alignment = 256;
			return info;
		}

	private:
		metal_buffer						_buffer{};
		u8*									_cpu_address{ nullptr };
		u32									_cpu_offset{ 0 };
		std::mutex							_mutex{};
	};

	struct metal_texture_init_info
	{
		MTL::Heap*							heap{ nullptr };
		MTL::Texture*						resource{ nullptr };
		MTL::TextureDescriptor*				texture_desc{ nullptr };
		MTL::ResourceOptions				allocation_info{};
		MTL::ClearColor						clear_value{};
	};

	class metal_texture
	{
	public:
		constexpr static u32 max_mips{ 14 }; // support up to 16k resolutions.
		metal_texture() = default;
		explicit metal_texture(metal_texture_init_info info);
		DISABLE_COPY(metal_texture);

		constexpr metal_texture(metal_texture&& other) noexcept
			: _texture(other._texture)
		{
			other.reset();
		}

		constexpr metal_texture& operator=(metal_texture&& other) noexcept
		{
			assert(this != &other);
			if (this != &other)
			{
				release();
				move(other);
			}
			return *this;
		}
		~metal_texture() { release(); }
		void release();
		[[nodiscard]] constexpr MTL::Texture* const texture() const { return _texture; }
	private:
		constexpr void move(metal_texture& other)
		{
			_texture = other._texture;
			other.reset();
		}

		constexpr void reset()
		{
			_texture = nullptr;
		}

		MTL::Texture*						_texture{ nullptr };
	};

	// Depth texture is the same as render target
	// Only difference is that it has a different usage flag.
	class metal_render_texture
	{
	public:
		metal_render_texture() = default;
		explicit metal_render_texture(metal_texture_init_info info);
		DISABLE_COPY(metal_render_texture);
		constexpr metal_render_texture(metal_render_texture&& other) noexcept
			: _texture{ std::move(other._texture) }, _mip_count(other._mip_count)
		{
			other.reset();
		}

		constexpr metal_render_texture& operator=(metal_render_texture&& other) noexcept
		{
			assert(this!= &other);
			if (this!= &other)
			{
				release();
				move(other);
			}
			return *this;
		}

		~metal_render_texture() { release(); }

		void release();
		[[nodiscard]] constexpr MTL::Texture* const texture() const { return _texture.texture(); }
		[[nodiscard]] constexpr u32 mip_count() const { return _mip_count; }
	private:
		constexpr void move(metal_render_texture& other)
		{
			_texture = std::move(other._texture);
			_mip_count = other._mip_count;
			other.reset();
		}
		
		constexpr void reset()
		{
			_mip_count = 0;
		}

		metal_texture			_texture{};
		u32						_mip_count{ 0 };
	};
}
#endif // _METAL_RESOURCE_H_