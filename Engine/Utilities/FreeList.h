#pragma once

#include "CommonHeaders.h"
#include <cstdio>
#include <algorithm>

namespace primal::utl
{

#if USE_STL_VECTOR
#pragma message("WARNING: using utl::fre_list with std::vector results in duplicate calls to class destructor!")
#endif

	template <typename T>
	class free_list
	{
		static_assert(sizeof(T) >= sizeof(u32));
	public:
		free_list() = default;
		explicit free_list(u32 count)
		{
			_array.reserve(count);
		}

		~free_list()
		{
			assert(!_size);
#if USE_STL_VECTOR
			memset(_array.data(), 0, _array.size() * sizeof(T));
#endif
		}

		template<class... params>
		constexpr u32 add(params&&... p)
		{
			u32 id{ u32_invalid_id };
			if (_next_free_index == u32_invalid_id)
			{
				id = (u32)_array.size();
				_array.emplace_back(std::forward<params>(p)...);
			}
			else
			{
				id = _next_free_index;
                if (!(id < _array.size() && already_removed(id, true))) {
                    printf("FreeList Corruption Detected! T: %s, size: %zu\n", typeid(T).name(), sizeof(T));
                    printf("id: %u, size: %u, next_free: %u\n", id, (unsigned)_array.size(), _next_free_index);
                    if (id < _array.size()) {
                         const u8 *const p{ (const u8 *const)std::addressof(_array[id]) };
                         // Find first non-CC byte starting from offset 4
                         int first_corrupt = -1;
                         for(size_t k = 4; k < sizeof(T); ++k) {
                             if (p[k] != 0xcc) {
                                 first_corrupt = (int)k;
                                 break;
                             }
                         }
                         if (first_corrupt != -1) {
                             printf("First corruption at offset %d: 0x%02X\n", first_corrupt, p[first_corrupt]);
                             // Dump surrounding bytes
                             printf("Dump around offset %d: ", first_corrupt);
                             int start = std::max(0, first_corrupt - 8);
                             int end = std::min((int)sizeof(T), first_corrupt + 8);
                             for(int k=start; k<end; ++k) printf("%02X ", p[k]);
                             printf("\n");
                         } else {
                             printf("No corruption found in byte scan? already_removed logic mismatch?\n");
                         }
                    }
                }
				assert(id < _array.size() && already_removed(id, true));
				_next_free_index = *(const u32 *const)std::addressof(_array[id]);
				new (std::addressof(_array[id])) T(std::forward<params>(p)...);
			}
			++_size;
			return id;
		}

		constexpr void remove(u32 id)
		{
            if (!(id < _array.size() && !already_removed(id, false))) {
                 printf("FreeList Remove Error! T: %s, id: %u, size: %u\n", typeid(T).name(), id, (unsigned)_array.size());
            }
			assert(id < _array.size() && !already_removed(id, false));
			T& item{ _array[id] };
			item.~T();
			DEBUG_OP(memset((void*)std::addressof(_array[id]), 0xcc, sizeof(T)));
			*(u32 *const)std::addressof(_array[id]) = _next_free_index;
			_next_free_index = id;
			--_size;
		}

        constexpr void reserve(u32 count)
        {
            _array.reserve(count);
        }

		constexpr u32 size() const
		{
			return _size;
		}

		constexpr u32 capacity() const
		{
			return (u32)_array.size();
		}

		constexpr bool empty() const
		{
			return _size == 0;
		}

		constexpr bool is_valid(u32 id) const
		{
			if (id >= _array.size()) return false;
			return !already_removed(id, false);
		}

		[[nodiscard]] constexpr T& operator[](u32 id)
		{
			assert(id < _array.size() && !already_removed(id, false));
			return _array[id];
		}

		[[nodiscard]] constexpr const T& operator[](u32 id) const
		{
			assert(id < _array.size() && !already_removed(id, false));
			return _array[id];
		}

	private:
		constexpr bool already_removed(u32 id, bool return_value_when_sizeof_t_equals_4) const
		{
			// NORE: when sizeof(T) == sizeof(u32) we can't test if the item was already removed!
			if constexpr (sizeof(T) > sizeof(u32))
			{
				u32 i{ sizeof(u32) };// skip the first 4 bytes.
				const u8 *const p{ (const u8 *const)std::addressof(_array[id]) };
				while((p[i] == 0xcc) && (i < sizeof(T))) ++i;
				return i == sizeof(T);
			}
			else
			{
				return return_value_when_sizeof_t_equals_4;
			}
		}
#if USE_STL_VECTOR
		utl::vector<T>							_array;
#else
		utl::vector<T, false>					_array;
#endif
		u32										_next_free_index{ u32_invalid_id };
		u32										_size{ 0 };
	};



}
