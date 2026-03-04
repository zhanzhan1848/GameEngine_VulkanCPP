#include "ContentToEngine.h"
#include "Graphics/Renderer.h"
#include "Utilities/IOStream.h"
#include <iostream>

// RHI Headers
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIMeshAsset.h"
#include "Graphics/RHI/Core/RHIGpuMesh.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalTexture.h"

// Metal Headers for parsing and upload
#ifdef __APPLE__
#include <Metal/Metal.hpp>
#endif

namespace primal::graphics::rhi {
    extern RHIDeviceManager g_deviceManager;
}

namespace primal::content
{
	namespace
	{
		class geometry_hierarchy_stream
		{
		public:
			DISABLE_COPY_AND_MOVE(geometry_hierarchy_stream);
			explicit geometry_hierarchy_stream(u8 *const buffer, u32 lods = u32_invalid_id)
			{
				assert(buffer && lods);
				if (lods != u32_invalid_id)
				{
					*((u32*)buffer) = lods;
				}
				_lod_count = *((u32*)buffer);
				_thresholds = (f32*)(&buffer[sizeof(u32)]);
				_lod_offsets = (lod_offset*)(&_thresholds[_lod_count]);
				_gpu_ids = (id::id_type*)(&_lod_offsets[_lod_count]);
			}

			void gpu_ids(u32 lod, id::id_type*& ids, u32& id_count)
			{
				assert(lod < _lod_count);
				ids = &_gpu_ids[_lod_offsets[lod].offset];
				id_count = _lod_offsets[lod].count;
			}

			u32 lod_from_threshold(f32 threshold)
			{
				assert(threshold >= 0);
				if (_lod_count == 1) return 0;
				for (u32 i{ _lod_count - 1 }; i >= 0; --i)
				{
					if (_thresholds[i] <= threshold) return i;
				}
				return 0;
			}

			[[nodiscard]] constexpr u32 lod_count() const { return _lod_count; }
			[[nodiscard]] constexpr f32* thresholds() const { return _thresholds; }
			[[nodiscard]] constexpr lod_offset* lod_offsets() const { return _lod_offsets; }
			[[nodiscard]] constexpr id::id_type* gpu_ids() const { return _gpu_ids; }

		private:
			f32*										_thresholds;
			lod_offset*									_lod_offsets;
			id::id_type*								_gpu_ids;
			u32											_lod_count;
		};

		// NOTE: This is needed to maintain compatibility with STL vector
		struct noexcept_map
		{
			std::unordered_map<u32, std::unique_ptr<u8[]>>				map;
			noexcept_map() = default;
			noexcept_map(const noexcept_map&) = default;
			noexcept_map(noexcept_map&&) noexcept = default;
			noexcept_map& operator=(const noexcept_map&) = default;
			noexcept_map& operator=(noexcept_map&&) noexcept = default;
		};

		// This constant indicates that an element in geometry_hierarchy is not a pointer bu a gpu_id
		constexpr uintptr_t								single_mesh_marker{ (uintptr_t)0x01 };
		utl::free_list<u8*>								geometry_hierarchies;
		std::mutex										geometry_mutex;

		utl::free_list<noexcept_map>					shader_groups;
		utl::free_list<std::unique_ptr<u8[]>>			shaders;
		std::mutex										shader_mutex;
#if defined(__APPLE__)
		std::unordered_map<id::id_type, std::string>	shader_function_name;
#endif

		// RHI Texture Map
        std::mutex& rhi_texture_mutex() {
            static std::mutex m;
            return m;
        }

        std::unordered_map<id::id_type, graphics::rhi::ResourceHandle>& rhi_texture_map() {
            static std::unordered_map<id::id_type, graphics::rhi::ResourceHandle> m;
            return m;
        }

        std::atomic<u32>                                rhi_texture_id_counter{ 0x80000000 };

		// RHI Mesh Assets
		utl::free_list<graphics::rhi::RHIMeshAsset>     rhi_mesh_assets;
		std::mutex                                      rhi_mesh_mutex;

        // RHI GPU Meshes (created on demand)
        std::unordered_map<id::id_type, std::unique_ptr<graphics::rhi::RHIGpuMesh>> rhi_gpu_meshes;
        std::mutex                                      rhi_gpu_mesh_mutex;

		void skip_mesh_in_blob(utl::blob_stream_reader& blob)
		{
			// Name
			const u32 name_len{ blob.read<u32>() };
			blob.skip(name_len);
			
			// Read headers to calculate skip sizes
			blob.skip(sizeof(u32)); // lod_id
			blob.skip(sizeof(u32)); // material_idx
			const u32 elem_size{ blob.read<u32>() };
			blob.skip(sizeof(u32)); // elem_type
			const u32 num_vertices{ blob.read<u32>() };
			const u32 index_size{ blob.read<u32>() };
			const u32 num_indices{ blob.read<u32>() };
			blob.skip(sizeof(f32)); // threshold

			// Pos Buffer
			blob.skip(12 * num_vertices);
			// Elem Buffer
			blob.skip(elem_size * num_vertices);
			// Index Buffer
			blob.skip(index_size * num_indices);

			// Meshlets
			blob.skip(sizeof(u32)); // magic_mshl
			const u32 meshlet_count{ blob.read<u32>() };
			blob.skip(meshlet_count * sizeof(graphics::rhi::RHIMeshlet));
			
			const u32 meshlet_vert_count{ blob.read<u32>() };
			blob.skip(meshlet_vert_count * sizeof(u32));
			
			const u32 meshlet_tri_count{ blob.read<u32>() };
			blob.skip(meshlet_tri_count * sizeof(u8));
			
			// SDF
			blob.skip(sizeof(u32)); // magic_sdf
			blob.skip(sizeof(u32) * 3); // Res
			blob.skip(sizeof(f32) * 6); // Bounds
			const u32 sdf_data_size{ blob.read<u32>() };
			blob.skip(sdf_data_size * sizeof(u16));
			
			const u32 voxel_size{ blob.read<u32>() };
			blob.skip(voxel_size * sizeof(u8));
			
			const u32 vector_field_size{ blob.read<u32>() };
			blob.skip(vector_field_size * sizeof(u16));
		}

		void parse_mesh_to_asset(utl::blob_stream_reader& blob, graphics::rhi::RHIMeshAsset& asset)
		{
			// Name (skip for now, RHIMeshAsset doesn't store name? Or maybe it should?)
			// RHIMeshAsset doesn't have name field in my definition.
			const u32 name_len{ blob.read<u32>() };
			blob.skip(name_len);

			asset.lod_id = blob.read<u32>();
			asset.material_idx = blob.read<u32>();
			const u32 elem_size{ blob.read<u32>() };
			asset.elements_type = blob.read<u32>();
			asset.num_vertices = blob.read<u32>();
			asset.index_size = blob.read<u32>();
			asset.num_indices = blob.read<u32>();
			asset.lod_threshold = blob.read<f32>();

			// Buffers
			asset.position_buffer.resize(12 * asset.num_vertices);
			memcpy(asset.position_buffer.data(), blob.position(), asset.position_buffer.size());
			blob.skip((u32)asset.position_buffer.size());

			asset.element_buffer.resize(elem_size * asset.num_vertices);
			memcpy(asset.element_buffer.data(), blob.position(), asset.element_buffer.size());
			blob.skip((u32)asset.element_buffer.size());

			asset.index_buffer.resize(asset.index_size * asset.num_indices);
			memcpy(asset.index_buffer.data(), blob.position(), asset.index_buffer.size());
			blob.skip((u32)asset.index_buffer.size());

			// Meshlets
			const u32 magic_mshl{ blob.read<u32>() };
			assert(magic_mshl == 0x4C48534D); // "MSHL"
			const u32 meshlet_count{ blob.read<u32>() };
			
#ifdef _DEBUG
			if (meshlet_count > 0) {
				// Verify consistency: index count should be multiple of 3
				// Note: We don't have the exact index count per meshlet here easily without iterating, 
				// but we can check the total triangle count from the next field if we read it.
				// Let's peek ahead or verify after reading.
			}
#endif

			asset.meshlets.resize(meshlet_count);
			if (meshlet_count > 0)
			{
				memcpy(asset.meshlets.data(), blob.position(), meshlet_count * sizeof(graphics::rhi::RHIMeshlet));
				blob.skip(meshlet_count * sizeof(graphics::rhi::RHIMeshlet));
			}

			const u32 meshlet_vert_count{ blob.read<u32>() };
			asset.meshlet_vertices.resize(meshlet_vert_count);
			if (meshlet_vert_count > 0)
			{
				memcpy(asset.meshlet_vertices.data(), blob.position(), meshlet_vert_count * sizeof(u32));
				blob.skip(meshlet_vert_count * sizeof(u32));
			}

			const u32 meshlet_tri_count{ blob.read<u32>() };
			asset.meshlet_triangles.resize(meshlet_tri_count);
			if (meshlet_tri_count > 0)
			{
				memcpy(asset.meshlet_triangles.data(), blob.position(), meshlet_tri_count * sizeof(u8));
				blob.skip(meshlet_tri_count * sizeof(u8));
			}

#ifdef _DEBUG
            // Validate Meshlet Data
            if (meshlet_count > 0) {
                // Check 1: Triangle count consistency
                // Each u8 represents a triangle index, so total u8s is simply the count.
                // But wait, meshlet_triangles are packed 3 indices per triangle or just indices?
                // Usually meshlet indices are u8 indices into the meshlet_vertices array.
                // If it's a triangle list, it should be divisible by 3.
                assert(meshlet_tri_count % 3 == 0 && "Meshlet triangle indices must be a multiple of 3");
            }
#endif

			// SDF
			const u32 magic_sdf{ blob.read<u32>() };
			assert(magic_sdf == 0x20464453); // "SDF "
			memcpy(asset.sdf.resolution, blob.position(), sizeof(u32) * 3);
			blob.skip(sizeof(u32) * 3);
			memcpy(asset.sdf.bounds_min, blob.position(), sizeof(f32) * 3);
			blob.skip(sizeof(f32) * 3);
			memcpy(asset.sdf.bounds_max, blob.position(), sizeof(f32) * 3);
			blob.skip(sizeof(f32) * 3);

			const u32 sdf_data_size{ blob.read<u32>() };
#ifdef _DEBUG
            // Validate SDF Size
            u32 expected_size = asset.sdf.resolution[0] * asset.sdf.resolution[1] * asset.sdf.resolution[2];
            // sdf_data_size is number of u16 elements
            if (sdf_data_size > 0) {
                assert(sdf_data_size == expected_size && "SDF data size mismatch");
            }
#endif
			asset.sdf.data.resize(sdf_data_size);
			if (sdf_data_size > 0)
			{
				memcpy(asset.sdf.data.data(), blob.position(), sdf_data_size * sizeof(u16));
				blob.skip(sdf_data_size * sizeof(u16));
			}

			const u32 voxel_size{ blob.read<u32>() };
#ifdef _DEBUG
            // Validate Voxel Size
            if (voxel_size > 0) {
                assert(voxel_size == expected_size && "Voxel data size mismatch");
            }
#endif
			asset.sdf.voxels.resize(voxel_size);
			if (voxel_size > 0)
			{
				memcpy(asset.sdf.voxels.data(), blob.position(), voxel_size * sizeof(u8));
				blob.skip(voxel_size * sizeof(u8));
			}

			const u32 vector_field_size{ blob.read<u32>() };
#ifdef _DEBUG
            // Validate Vector Field Size
            if (vector_field_size > 0) {
                // Vector field is 4 * u16 per voxel? 
                // ContentTools wrote: sizeof(u16) * m.sdf.vector_field.size()
                // And vector_field in RHIMeshAsset is vector<u16>.
                // So size should be resolution.x * y * z * 4
                assert(vector_field_size == expected_size * 4 && "Vector field size mismatch");
            }
#endif
			asset.sdf.vector_field.resize(vector_field_size);
			if (vector_field_size > 0)
			{
				memcpy(asset.sdf.vector_field.data(), blob.position(), vector_field_size * sizeof(u16));
				blob.skip(vector_field_size * sizeof(u16));
			}
		}

		// NOTE: Expects the same data as create_geometry_resource()
		u32 get_geometry_hierarchy_buffer_size(const void *const data)
		{
			assert(data);
			utl::blob_stream_reader blob{ (const u8*)data };
			const u32 lod_count{ blob.read<u32>() };
			assert(lod_count);
			// add size or lod_count, thresholds and lod offsets to the size of hierarchy.
#if defined(_WIN32)
			u32 size{ sizeof(u32) + (sizeof(f32) + sizeof(lod_offset)) * lod_count };
#elif defined(__APPLE__)
			u32 size{ static_cast<u32>(sizeof(u32) + (sizeof(f32) + sizeof(lod_offset)) * lod_count) };
#endif

			for (u32 lod_idx{ 0 }; lod_idx < lod_count; ++lod_idx)
			{
				// skip threshold
				blob.skip(sizeof(f32));
				// add size of gpu_ids (sizeof(id::id_type) * submesh_count))
				const u32 id_count{ blob.read<u32>() };
				size += sizeof(id::id_type) * id_count;
				// skip submesh data and go to the next LOD
				blob.skip(sizeof(u32)); // skip size_of_submeshes

				for (u32 i{ 0 }; i < id_count; ++i)
				{
					skip_mesh_in_blob(blob);
				}
			}

			return size;
		}

		// Creates a hierarchy stream for a geometry that has multiple LODs and/or multiple submeshes.
		// NOTE: Expects the same data as create_geometry_resource()
		id::id_type create_mesh_hierarchy(const void *const data)
		{
			assert(data);
			std::cout << "create_mesh_hierarchy called." << std::endl;
			const u32 size{ get_geometry_hierarchy_buffer_size(data) };
			u8 *const hierarchy_buffer{ (u8 *const)malloc(size) };

			utl::blob_stream_reader blob{ (const u8*)data };
			const u32 lod_count{ blob.read<u32>() };
			assert(lod_count);
			geometry_hierarchy_stream stream{ hierarchy_buffer, lod_count };
			u32 submesh_index{ 0 };
			id::id_type *const gpu_ids{ stream.gpu_ids() };

			for (u32 lod_idx{ 0 }; lod_idx < lod_count; ++lod_idx)
			{
				stream.thresholds()[lod_idx] = blob.read<f32>();
				const u32 id_count{ blob.read<u32>() };
				assert(id_count < (1 << 16));
				stream.lod_offsets()[lod_idx] = { (u16)submesh_index, (u16)id_count };
				blob.skip(sizeof(u32)); // skip over size_of_submeshes
				for (u32 id_idx{ 0 }; id_idx < id_count; ++id_idx)
				{
					const u8* at{ blob.position() };
					std::cout << "create_mesh_hierarchy: calling graphics::add_submesh for LOD " << lod_idx << ", id " << id_idx << std::endl;
					gpu_ids[submesh_index++] = graphics::add_submesh(at);
					skip_mesh_in_blob(blob);
					assert(submesh_index < (1 << 16));
				}
			}

			assert([&]() 
			{
				f32 previous_threshold{ stream.thresholds()[0] };
				for (u32 i{ 1 }; i < lod_count; ++i)
				{
					if (stream.thresholds()[i] <= previous_threshold) return false;
					previous_threshold = stream.thresholds()[i];
				}
				return true;
			}());

			static_assert(alignof(void*) > 2, "We need the least significant bit for the single mesh marker.");
			std::lock_guard lock{ geometry_mutex };
			return geometry_hierarchies.add(hierarchy_buffer);
		}

		// Create a single submesh gpu_id
		// NOTE: Expects the same data as create_geometry_resource()
		id::id_type create_single_submesh(const void *const data)
		{
			assert(data);
			std::cout << "create_single_submesh called." << std::endl;
			utl::blob_stream_reader blob{ (const u8*)data };
			// skip lod_count, lod_threshold, submesh_count and size_of_submeshes
			blob.skip(sizeof(u32) + sizeof(f32) + sizeof(u32) + sizeof(u32));
			const u8* at{ blob.position() };
			std::cout << "create_single_submesh: calling graphics::add_submesh..." << std::endl;
			const id::id_type gpu_id{ graphics::add_submesh(at) };
			std::cout << "create_single_submesh: graphics::add_submesh returned " << gpu_id << std::endl;

			// create a fake pointer and put it in the geometry hierarchies
			static_assert(sizeof(uintptr_t) > sizeof(id::id_type));
			constexpr u8 shift_bits{ (sizeof(uintptr_t) - sizeof(id::id_type)) << 3 };
			u8 *const fake_pointer{ (u8 *const)((((uintptr_t)gpu_id) << shift_bits) | single_mesh_marker) };
			std::lock_guard lock{ geometry_mutex };
			return geometry_hierarchies.add(fake_pointer);
		}

		// RHI Implementation
		id::id_type create_rhi_single_submesh(const void *const data)
		{
			assert(data);
			utl::blob_stream_reader blob{ (const u8*)data };
			// skip lod_count, lod_threshold, submesh_count and size_of_submeshes
			blob.skip(sizeof(u32) + sizeof(f32) + sizeof(u32) + sizeof(u32));
			
			graphics::rhi::RHIMeshAsset asset;
			parse_mesh_to_asset(blob, asset);
			
			std::lock_guard lock{ rhi_mesh_mutex };
			const id::id_type rhi_id{ rhi_mesh_assets.add(asset) };

			// create a fake pointer and put it in the geometry hierarchies
			static_assert(sizeof(uintptr_t) > sizeof(id::id_type));
			constexpr u8 shift_bits{ (sizeof(uintptr_t) - sizeof(id::id_type)) << 3 };
			u8 *const fake_pointer{ (u8 *const)((((uintptr_t)rhi_id) << shift_bits) | single_mesh_marker) };
			std::lock_guard geometry_lock{ geometry_mutex };
			return geometry_hierarchies.add(fake_pointer);
		}

		id::id_type create_rhi_mesh_hierarchy(const void *const data)
		{
			assert(data);
			const u32 size{ get_geometry_hierarchy_buffer_size(data) };
			u8 *const hierarchy_buffer{ (u8 *const)malloc(size) };

			utl::blob_stream_reader blob{ (const u8*)data };
			const u32 lod_count{ blob.read<u32>() };
			assert(lod_count);
			geometry_hierarchy_stream stream{ hierarchy_buffer, lod_count };
			u32 submesh_index{ 0 };
			id::id_type *const rhi_ids{ stream.gpu_ids() }; // Reuse gpu_ids pointer for rhi_ids

			for (u32 lod_idx{ 0 }; lod_idx < lod_count; ++lod_idx)
			{
				stream.thresholds()[lod_idx] = blob.read<f32>();
				const u32 id_count{ blob.read<u32>() };
				assert(id_count < (1 << 16));
				stream.lod_offsets()[lod_idx] = { (u16)submesh_index, (u16)id_count };
				blob.skip(sizeof(u32)); // skip over size_of_submeshes
				for (u32 id_idx{ 0 }; id_idx < id_count; ++id_idx)
				{
					graphics::rhi::RHIMeshAsset asset;
					parse_mesh_to_asset(blob, asset);
					
					std::lock_guard lock{ rhi_mesh_mutex };
					rhi_ids[submesh_index++] = rhi_mesh_assets.add(asset);
					
					assert(submesh_index < (1 << 16));
				}
			}

			// Validate thresholds (same as legacy)
			assert([&]() 
			{
				f32 previous_threshold{ stream.thresholds()[0] };
				for (u32 i{ 1 }; i < lod_count; ++i)
				{
					if (stream.thresholds()[i] <= previous_threshold) return false;
					previous_threshold = stream.thresholds()[i];
				}
				return true;
			}());

			static_assert(alignof(void*) > 2, "We need the least significant bit for the single mesh marker.");
			std::lock_guard lock{ geometry_mutex };
			return geometry_hierarchies.add(hierarchy_buffer);
		}

		// Determine if this geometry has a single lod with a single submesh.
		// NOTE: Expects the same data as create_geometry_resource()
		bool is_single_mesh(const void *const data)
		{
			assert(data);
			utl::blob_stream_reader blob{ (const u8*)data };
			const u32 lod_count{ blob.read<u32>() };
			assert(lod_count);
			if (lod_count > 1) return false;

			// skip over threshold
			blob.skip(sizeof(f32));
			const u32 submesh_count{ blob.read<u32>() };
			assert(submesh_count);
			return submesh_count == 1;
		}

#if defined(_WIN32)
		constexpr id::id_type gpu_id_from_fake_pointer(u8 *const pointer)
		{
			assert((uintptr_t)pointer & single_mesh_marker);
			static_assert(sizeof(uintptr_t) > sizeof(id::id_type));
			constexpr u8 shift_bits{ (sizeof(uintptr_t) - sizeof(id::id_type)) << 3 };
			return (((uintptr_t)pointer) >> shift_bits) & (uintptr_t)id::invalid_id; // '& (uintptr_t)id::invalid_id' is to clear the higher bits. Probably not necessary for x64
		}
#elif defined(__APPLE__)
		id::id_type gpu_id_from_fake_pointer(u8 *const pointer)
		{
			assert((uintptr_t)pointer & single_mesh_marker);
			static_assert(sizeof(uintptr_t) > sizeof(id::id_type));
			constexpr u8 shift_bits{ (sizeof(uintptr_t) - sizeof(id::id_type)) << 3 };
			return (((uintptr_t)pointer) >> shift_bits) & (uintptr_t)id::invalid_id; // '& (uintptr_t)id::invalid_id' is to clear the higher bits. Probably not necessary for x64
		}
#endif

		// NOTE: Expects 'data' to contain:
		// struct
		// {
		//		u32 lod_count,
		//		struct
		//		{
		//			f32 lod_threshold,
		//			u32 submesh_count,
		//			u32	size_of_submeshes,
		//			struct
		//			{
		//				u32 element_size, u32 vertex_count,
		//				u32 index_count, u32 elements_type, u32 primitive_topology
		//				u8 positions[sizeof(f32) * 3 * vertex_count], // sizeof(positions) must be a multiple of 16 bytes. Pad if needed.
		//				u8 elements[sizeof(element_size) * vertex_count], // sizeof(elements) must be a multiple of 16 bytes. Pad if needed.
		//				u8 indices[index_size * index_count],
		//			} submeshes[index_size * index_count]
		//		} mesh_lods[mod_count]
		//	} geometry;
		//
		// Output format
		// 
		// If geometry has more than one LOD oir submesh
		// struct
		// {
		//		u32 lod_count,
		//		f32 thresholds[lod_count]
		//		struct
		//		{
		//			u16 offset,
		//			u16 count
		//		} lod_offsets[lod_count],
		//		id::id_type gpu_ids[total_number_of_submeshes]
		//	} geometry_hierarchy
		// 
		// If geometry has a single LOD and submesh
		// 
		// (gpu_id << 32) | 0x01
		//

		[[nodiscard]] id::id_type create_geometry_resource(const void *const data)
		{
			assert(data);
			std::cout << "create_geometry_resource: Checking is_single_mesh..." << std::endl;
			bool single = is_single_mesh(data);
			std::cout << "create_geometry_resource: is_single_mesh = " << single << std::endl;
			return single ? create_single_submesh(data) : create_mesh_hierarchy(data);
		}

		[[nodiscard]] id::id_type create_rhi_geometry_resource(const void* const data)
		{
			return is_single_mesh(data) ? create_rhi_single_submesh(data) : create_rhi_mesh_hierarchy(data);
		}

		void destory_geometry_resource(id::id_type id)
		{
			std::lock_guard lock{ geometry_mutex };
			u8 *const pointer{ geometry_hierarchies[id] };

			if ((uintptr_t)pointer & single_mesh_marker)
			{
				graphics::remove_submesh(gpu_id_from_fake_pointer(pointer));
			}
			else
			{
				geometry_hierarchy_stream stream{ pointer };
				const u32 lod_count{ stream.lod_count() };
				u32 id_index{ 0 };
				for (u32 lod{ 0 }; lod < lod_count; ++lod)
				{
					for (u32 i{ 0 }; i < stream.lod_offsets()[lod].count; ++i)
					{
						graphics::remove_submesh(stream.gpu_ids()[id_index++]);
					}
				}
				free(pointer);
			}


			geometry_hierarchies.remove(id);
		}

		void destroy_rhi_geometry_resource(id::id_type id)
		{
			std::lock_guard lock{ geometry_mutex };
			u8* const pointer{ geometry_hierarchies[id] };
			if ((uintptr_t)pointer & single_mesh_marker)
			{
				static_assert(sizeof(uintptr_t) > sizeof(id::id_type));
				constexpr u8 shift_bits{ (sizeof(uintptr_t) - sizeof(id::id_type)) << 3 };
				const id::id_type rhi_id{ (id::id_type)((uintptr_t)pointer >> shift_bits) };
				
				{
					std::lock_guard rhi_lock{ rhi_mesh_mutex };
					rhi_mesh_assets.remove(rhi_id);
				}

                {
                    std::lock_guard gpu_lock{ rhi_gpu_mesh_mutex };
                    rhi_gpu_meshes.erase(rhi_id);
                }
			}
			else
			{
				geometry_hierarchy_stream stream{ pointer };
				const u32 lod_count{ stream.lod_count() };
				u32 id_index{ 0 };
				for (u32 lod{ 0 }; lod < lod_count; ++lod)
				{
					for (u32 i{ 0 }; i < stream.lod_offsets()[lod].count; ++i)
					{
						const id::id_type rhi_id{ stream.gpu_ids()[id_index++] };
						
						{
							std::lock_guard rhi_lock{ rhi_mesh_mutex };
							rhi_mesh_assets.remove(rhi_id);
						}

                        {
                            std::lock_guard gpu_lock{ rhi_gpu_mesh_mutex };
                            rhi_gpu_meshes.erase(rhi_id);
                        }
					}
				}
				free(pointer);
			}
			geometry_hierarchies.remove(id);
		}

		// NOTE: expects data to contain
		// struct{
		//	material_type::type type;
		//	u32					texture_count;
		//  id::id_type			shader_ids[shader_type::count];
		//  id::id_type*		texture_ids;
		//	} material_init_info;
		[[nodiscard]] id::id_type create_material_resource(const void *const data)
		{
			assert(data);
			return graphics::add_material(*(const graphics::material_init_info *const)data);
		}

		void destory_material_resource(id::id_type id)
		{
			graphics::remove_material(id);
		}

		// NOTE: expects data to contain
		// struct {
		//         u32 width, height, array_size(or depth), flags, mip_levels, format,
		//         struct{
		//             u32 row_pitch, slice_pitch,
		//             u8 image[mip_level][slice_pitch * mip_per_depth],
		//         } images[]
		// } texture
		[[nodiscard]] id::id_type create_texture_resource(const void *const data)
		{
			assert(data);

			// Try RHI first
			if (graphics::rhi::g_deviceManager.GetDeviceCount() > 0) {
				auto* device = graphics::rhi::g_deviceManager.GetDevice(1); 
				if (device && device->GetDesc().platform == graphics::rhi::RHIPlatform::Metal) {
#ifdef __APPLE__
					utl::blob_stream_reader blob((const u8*)data);
					const u32 width{ blob.read<u32>() };
					const u32 height{ blob.read<u32>() };
					const u32 array_size{ blob.read<u32>() };
					const u32 flags{ blob.read<u32>() };
					const u32 mip_levels{ blob.read<u32>() };
					const u32 format_u32{ blob.read<u32>() };

					graphics::rhi::TextureDesc desc{};
					desc.size = { width, height, 1 }; // Assuming 2D
					desc.arraySize = array_size;
					desc.mipLevels = mip_levels;
					desc.type = (array_size > 1) ? graphics::rhi::TextureType::Texture2DArray : graphics::rhi::TextureType::Texture2D;
					// Map format from DXGI (TextureImporter) to RHI DataFormat
			// DXGI_FORMAT_R8G8B8A8_UNORM = 28
			// DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29
			// DXGI_FORMAT_BC1_UNORM = 71
			// DXGI_FORMAT_BC1_UNORM_SRGB = 72
			// DXGI_FORMAT_BC7_UNORM = 98
			// DXGI_FORMAT_BC7_UNORM_SRGB = 99
			if (format_u32 == 28) desc.format = graphics::rhi::DataFormat::RGBA8_UNorm;
			else if (format_u32 == 29) desc.format = graphics::rhi::DataFormat::RGBA8_sRGB;
			else if (format_u32 == 71) desc.format = graphics::rhi::DataFormat::BC1_UNorm;
			else if (format_u32 == 72) desc.format = graphics::rhi::DataFormat::BC1_sRGB;
			else if (format_u32 == 98) desc.format = graphics::rhi::DataFormat::BC7_UNorm;
			else if (format_u32 == 99) desc.format = graphics::rhi::DataFormat::BC7_sRGB;
			else desc.format = graphics::rhi::DataFormat::RGBA8_UNorm; // Fallback
					
					desc.usage = graphics::rhi::TextureUsage::ShaderResource | graphics::rhi::TextureUsage::CopyDest;

					auto handle = device->CreateTexture(desc);
					if (handle == graphics::rhi::handles::INVALID_RESOURCE) {
                        std::cerr << "Failed to create RHI texture." << std::endl;
						return id::invalid_id;
					}

					auto* metalDevice = static_cast<graphics::rhi::MetalDevice*>(device);
					auto* texture = metalDevice->GetTexture(handle);
					if (texture) {
						auto* mtlTexture = texture->GetNativeTexture();
                        if (!mtlTexture) {
                            std::cerr << "Failed to get native Metal texture." << std::endl;
                        } else {
                            std::cout << "Got native Metal texture: " << mtlTexture << std::endl;
                        }
						
						for (u32 i{ 0 }; i < array_size; ++i)
						{
							for (u32 j{ 0 }; j < mip_levels; ++j)
							{
								const u32 row_pitch{ blob.read<u32>() };
								const u32 slice_pitch{ blob.read<u32>() };
								
								// Check bounds
								u32 mipWidth = std::max(1u, width >> j);
								u32 mipHeight = std::max(1u, height >> j);

                                // std::cout << "Writing Mip " << j << " Slice " << i << " Size " << mipWidth << "x" << mipHeight << " Pitch " << row_pitch << " SlicePitch " << slice_pitch << std::endl;

                                if (mtlTexture) {
                                     NS::UInteger bytesPerImage = (array_size > 1) ? slice_pitch : 0;
                                     
                                     MTL::StorageMode mode = mtlTexture->storageMode();

                                     if (mode == MTL::StorageModePrivate) {
                                          // std::cout << "Texture is Private. Using Staging Buffer." << std::endl;
                                          // For Private textures, we must use a Staging Buffer and BlitEncoder
                                          auto* device = metalDevice->GetNativeDevice();
                                          if (!device) std::cerr << "Native Device is NULL!" << std::endl;
                                          
                                          MTL::Buffer* stagingBuffer = device->newBuffer(slice_pitch, MTL::ResourceStorageModeShared);
                                          if (stagingBuffer) {
                                              // std::cout << "Staging Buffer Created: " << stagingBuffer << std::endl;
                                              // std::cout << "Copying to staging buffer..." << std::endl;
                                              memcpy(stagingBuffer->contents(), blob.position(), slice_pitch);
                                              
                                              auto* queue = metalDevice->GetTransferQueue();
                                              if (!queue) std::cerr << "Transfer Queue is NULL!" << std::endl;
                                              
                                              auto* cmdBuffer = queue->commandBuffer();
                                              if (!cmdBuffer) std::cerr << "Command Buffer creation failed!" << std::endl;
                                              
                                              auto* blitEncoder = cmdBuffer->blitCommandEncoder();
                                              if (!blitEncoder) std::cerr << "Blit Encoder creation failed!" << std::endl;
                                              
                                              // std::cout << "Encoding Blit..." << std::endl;
                                              blitEncoder->copyFromBuffer(
                                                  stagingBuffer,
                                                  0,
                                                  row_pitch,
                                                  bytesPerImage,
                                                  MTL::Size::Make(mipWidth, mipHeight, 1),
                                                  mtlTexture,
                                                  i, // slice
                                                  j, // level,
                                                  MTL::Origin::Make(0, 0, 0)
                                              );
                                              
                                              blitEncoder->endEncoding();
                                              cmdBuffer->commit();
                                              // std::cout << "Waiting for completion..." << std::endl;
                                              cmdBuffer->waitUntilCompleted();
                                              // std::cout << "Upload Completed." << std::endl;
                                              
                                              stagingBuffer->release();
                                          } else {
                                              std::cerr << "Failed to create staging buffer for texture upload." << std::endl;
                                          }
                                      } else {
                                         // For Managed/Shared textures, we can use replaceRegion
    								     mtlTexture->replaceRegion(MTL::Region(0, 0, 0, mipWidth, mipHeight, 1), j, i, blob.position(), row_pitch, bytesPerImage);
                                     }
                                }
								
								blob.skip(slice_pitch);
							}
						}
					}
					
					id::id_type new_id = rhi_texture_id_counter++;
                    {
                        std::lock_guard lock(rhi_texture_mutex());
                        rhi_texture_map()[new_id] = handle;
                    }
                    return new_id;
#endif
				}
			}

			return graphics::add_texture((const u8 *const)data);
		}

		void destory_texture_resource(id::id_type id)
		{
			graphics::remove_texture(id);
		}

	} // anonymous namespace

	id::id_type create_resource(const void * const data, asset_type::type type, GraphicsAPI api)
	{
		assert(data);
		id::id_type id{ id::invalid_id };

		switch (type)
		{
		case asset_type::animation:																		break;
		case asset_type::audio:																			break;
		case asset_type::material:			id = create_material_resource(data);						break;
		case asset_type::mesh:				
			if (api == GraphicsAPI::Legacy)
				id = create_geometry_resource(data);
			else
				id = create_rhi_geometry_resource(data);
			break;
		case asset_type::skeleton:																		break;
		case asset_type::texture:			id = create_texture_resource(data);							break;
		case asset_type::unkonwn:																		break;
		case asset_type::count:																			break;
		}

		assert(id::is_valid(id));

		return id;
	}

    graphics::rhi::ResourceHandle get_rhi_texture_handle(id::id_type id)
    {
        std::lock_guard lock(rhi_texture_mutex());
        auto& map = rhi_texture_map();
        auto it = map.find(id);
        if (it != map.end()) {
            return it->second;
        }
        return graphics::rhi::handles::INVALID_RESOURCE;
    }

	bool get_rhi_mesh_asset(id::id_type id, graphics::rhi::RHIMeshAsset& asset)
	{
		std::lock_guard lock{ rhi_mesh_mutex };
		// Check if id is valid. free_list doesn't support direct existence check easily without potentially crashing if index is out of bounds?
		// Assuming id is valid if passed here. But we can catch exceptions if free_list throws.
		// Or simply assume the caller knows what they are doing.
		// However, we should try to be safe.
		// Since we don't have a simple 'contains' method for free_list without checking implementation,
		// we will assume valid ID for now, or check against size if possible.
		// But free_list reuses IDs.
		// Let's just access it.
		asset = rhi_mesh_assets[id];
		return true;
	}

    graphics::rhi::RHIGpuMesh* get_rhi_gpu_mesh(id::id_type id)
    {
        std::lock_guard lock{ rhi_gpu_mesh_mutex };
        auto it = rhi_gpu_meshes.find(id);
        if (it != rhi_gpu_meshes.end())
        {
            return it->second.get();
        }

        // Create new
        if (graphics::rhi::g_deviceManager.GetDeviceCount() == 0) return nullptr;
        
        // Get Asset
        graphics::rhi::RHIMeshAsset asset;
        {
            // Avoid deadlock? No, rhi_mesh_mutex is different.
            if (!get_rhi_mesh_asset(id, asset)) return nullptr;
        }

        auto* device = graphics::rhi::g_deviceManager.GetDevice(1); // Use device 1 as per convention in this file
        if (!device) return nullptr;

        auto mesh = std::make_unique<graphics::rhi::RHIGpuMesh>();
        if (!mesh->Initialize(*device, asset)) {
            return nullptr;
        }
        
        auto* result = mesh.get();
        rhi_gpu_meshes[id] = std::move(mesh);
        
        return result;
    }

    id::id_type register_mesh_asset(graphics::rhi::RHIMeshAsset& asset)
    {
        std::lock_guard lock{ rhi_mesh_mutex };
        return rhi_mesh_assets.add(std::move(asset));
    }

    void foreach_gpu_mesh(GpuMeshCallback callback)
    {
        std::lock_guard lock{ rhi_gpu_mesh_mutex };
        for (const auto& pair : rhi_gpu_meshes) {
            if (pair.second) {
                callback(pair.first, pair.second.get());
            }
        }
    }

	void destroy_resource(id::id_type id, asset_type::type type, GraphicsAPI api)
	{
		assert(id::is_valid(id));
		switch (type)
		{
		case asset_type::animation:																		break;
		case asset_type::audio:																			break;
		case asset_type::material:			destory_material_resource(id);								break;
		case asset_type::mesh:
			if (api == GraphicsAPI::Legacy)
				destory_geometry_resource(id);
			else
				destroy_rhi_geometry_resource(id);
			break;
		case asset_type::skeleton:																		break;
		case asset_type::texture:			destory_texture_resource(id);								break;
		default:
			assert(false);
			break;
		}
	}

    void shutdown()
    {
        // Clear GPU Meshes first as they depend on Device
        {
            std::lock_guard lock{ rhi_gpu_mesh_mutex };
            rhi_gpu_meshes.clear();
        }
        
        // Clear Textures
        {
            std::lock_guard lock(rhi_texture_mutex());
            rhi_texture_map().clear();
        }

        // Clear Content Resources
        {
            std::lock_guard lock{ rhi_mesh_mutex };
            rhi_mesh_assets.clear();
        }
        
        {
            std::lock_guard lock{ geometry_mutex };
            // Note: This leaks memory if geometry_hierarchies contained pointers to malloc'd memory
            // But since we are shutting down, it's acceptable to bypass the free_list assertion
            geometry_hierarchies.clear();
        }

        {
            std::lock_guard lock{ shader_mutex };
            shader_groups.clear();
            shaders.clear();
#if defined(__APPLE__)
            shader_function_name.clear();
#endif
        }
    }

	// NOTE: expect shaders to be an array of pointers to compiled_shaders
	// NOTE: the editor is responsible for making sure that there are no duplicate shaders. If there are, we'll happily add them!
	id::id_type add_shader_group(const u8* const* shaders, u32 num_shaders, const u32* const keys)
	{
		assert(shaders && num_shaders && keys);
		noexcept_map group;
		for (u32 i{ 0 }; i < num_shaders; ++i)
		{
			assert(shaders[i]);
			const compiled_shader_ptr shader_ptr{ (const compiled_shader_ptr)shaders[i] };
			const u64 size{ compiled_shader::buffer_size(shader_ptr->byte_code_size()) };
			std::unique_ptr<u8[]> shader{ std::make_unique<u8[]>(size) };
			memcpy(shader.get(), shaders[i], size);
			group.map[keys[i]] = std::move(shader);
		}
		std::lock_guard lock{ shader_mutex };
		return shader_groups.add(std::move(group));
	}

	void remove_shader_group(id::id_type id)
	{
		std::lock_guard lock{ shader_mutex };
		assert(id::is_valid(id));

		shader_groups[id].map.clear();
		shader_groups.remove(id);
	}

	compiled_shader_ptr get_shader(id::id_type id, u32 shader_key)
	{
		std::lock_guard lock{ shader_mutex };
		assert(id::is_valid(id));

		for (const auto& [key, value] : shader_groups[id].map)
		{
			if (key == shader_key)
			{
				return (const compiled_shader_ptr)value.get();
			}
		}
	}

	id::id_type add_shader(const u8 * data)
	{
		const compiled_shader_ptr shader_ptr{ (const compiled_shader_ptr)data };
		const u64 size{ sizeof(u64) + compiled_shader::hash_length + shader_ptr->byte_code_size() };
		std::unique_ptr<u8[]> shader{ std::make_unique<u8[]>(size) };
		memcpy(shader.get(), data, size);
		std::lock_guard lock{ shader_mutex };
		return shaders.add(std::move(shader));
	}

	void remove_shader(id::id_type id)
	{
		std::lock_guard lock{ shader_mutex };
		assert(id::is_valid(id));
		shaders.remove(id);
	}

	compiled_shader_ptr get_shader(id::id_type id)
	{
		std::lock_guard lock{ shader_mutex };
		assert(id::is_valid(id));
		return (const compiled_shader_ptr)(shaders[id].get());
	}

#if defined(__APPLE__)
	void add_shader_function_name(id::id_type shader_group_id, const char* name)
	{
		std::lock_guard lock{ shader_mutex };
		assert(id::is_valid(shader_group_id));
		shader_function_name[shader_group_id] = std::string{ name };
	}

	const char* get_shader_function_name(id::id_type shader_group_id)
	{
		std::lock_guard lock{ shader_mutex };
		assert(id::is_valid(shader_group_id));
		return shader_function_name[shader_group_id].c_str();
	}

	void remove_shader_function_name(id::id_type shader_group_id)
	{
		std::lock_guard lock{ shader_mutex };
		assert(id::is_valid(shader_group_id));
		shader_function_name[shader_group_id].clear();
	}
#endif

	void get_submesh_gpu_ids(id::id_type geometry_content_id, u32 id_count, id::id_type * const gpu_ids)
	{
		std::lock_guard lock{ geometry_mutex };
		u8 *const pointer{ geometry_hierarchies[geometry_content_id] };
		if ((uintptr_t)pointer & single_mesh_marker)
		{
			// assert(id_count == 1);
			*gpu_ids = gpu_id_from_fake_pointer(pointer);
		}
		else
		{
			geometry_hierarchy_stream stream{ pointer };
			assert([&]() {
			const u32 lod_count{ stream.lod_count() };
			const lod_offset lod_offset{ stream.lod_offsets()[lod_count - 1] };
			const u32 gpu_id_count{ (u32)lod_offset.offset + (u32)lod_offset.count };
			printf("lod_count: %u, offset: %u, count: %u, calculated gpu_id_count: %u, expected id_count: %u\n", 
				lod_count, lod_offset.offset, lod_offset.count, gpu_id_count, id_count);
			return gpu_id_count == id_count;
			}());

			memcpy(gpu_ids, stream.gpu_ids(), sizeof(id::id_type) * id_count);
		}
	}

	void get_lod_offsets(const id::id_type * const geometry_ids, const f32 * const thresholds, u32 id_count, utl::vector<lod_offset>& offsets)
	{
		assert(geometry_ids && thresholds && id_count);
		assert(offsets.empty());

		std::lock_guard lock{ geometry_mutex };

		for (u32 i{ 0 }; i < id_count; ++i)
		{
			u8 *const pointer{ geometry_hierarchies[geometry_ids[i]] };
			if ((uintptr_t)pointer & single_mesh_marker)
			{
				assert(id_count == 1);
				offsets.emplace_back(lod_offset{ 0, 1 });
			}
			else
			{
				geometry_hierarchy_stream stream{ pointer };
				const u32 lod{ stream.lod_from_threshold(thresholds[i]) };
				offsets.emplace_back(stream.lod_offsets()[lod]);
			}
		}
	}
	
}