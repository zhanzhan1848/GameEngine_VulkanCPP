#include "MetalContent.h"

#include "MetalResource.h"
#include "Utilities/IOStream.h"
#include "Content/ContentToEngine.h"
#include "MetalCore.h"
#include "MetalHelper.h"
#include "MetalGPass.h"

namespace primal::graphics::metal::content
{
	namespace
    {
        struct pso_id
		{
			id::id_type										gpass_pso_id{ id::invalid_id };
			id::id_type										depth_pso_id{ id::invalid_id };
		};

		struct submesh_view
		{
			buffer_view						         	   	position_buffer_view{};
			buffer_view						         	   	element_buffer_view{};
			buffer_view							        	index_buffer_view{};
			MTL::PrimitiveType							    primitive_topology;
			u32												elements_type{};
		};

        struct metal_render_item
		{
			id::id_type										entity_id;
			id::id_type										submesh_gpu_id;
			id::id_type										material_id;
			id::id_type										pso_id;
			id::id_type										depth_pso_id;
		};

        utl::free_list<MTL::Buffer*>						submesh_buffers{};
		utl::free_list<submesh_view>						submesh_views{};
		std::mutex											submesh_mutex{};

		utl::free_list<metal_texture>						textures;
		utl::free_list<u32>									descriptor_indices;
		std::mutex											texture_mutex{};

		utl::vector<NS::Array*>								argument_buffer_layouts;
		std::unordered_map<u64, id::id_type>				mtl_rs_map; // maps a material's type and shader flags to an index in the array of root signature
		utl::free_list<std::unique_ptr<u8[]>>				materials;
		std::mutex											material_mutex{};

		utl::free_list<metal_render_item>					render_items;
		utl::free_list<std::unique_ptr<id::id_type[]>>		render_item_ids;
		utl::vector<MTL::RenderPipelineState*>				pipeline_states;
		std::unordered_map<u64, id::id_type>				pso_map;
		std::mutex											render_item_mutex{};

		struct
		{
			utl::vector<primal::content::lod_offset>		lod_offsets;
			utl::vector<id::id_type>						geometry_ids;
		} frame_cache;

		// Like D3D12 Root Signature, but for Metal
		id::id_type create_arguments_buffer(material_type::type type, shader_flags::flags flags);

		class metal_material_stream
		{
		public:
			DISABLE_COPY_AND_MOVE(metal_material_stream);
			explicit metal_material_stream(u8 *const material_buffer) : _buffer{ material_buffer } { initialize(); }

			explicit metal_material_stream(std::unique_ptr<u8[]>& material_buffer, material_init_info info)
			{
				assert(!material_buffer);

				u32 shader_count{ 0 };
				u32 flags{ 0 };
				for (u32 i{ 0 }; i < shader_type::count; ++i)
				{
					if (id::is_valid(info.shader_ids[i]))
					{
						++shader_count;
						flags |= (1 << i);
					}
				}

				assert(shader_count && flags);

				const u32 buffer_size{ static_cast<u32>(
						sizeof(material_type::type) + 
						sizeof(shader_flags::flags) +
						sizeof(id::id_type) +
						sizeof(u32) +
						shader_count * sizeof(id::id_type) +
						sizeof(id::id_type) + sizeof(u32) * info.texture_count
					)
				};

				material_buffer = std::make_unique<u8[]>(buffer_size);
				_buffer = material_buffer.get();
				u8 *const buffer{ _buffer };

				*(material_type::type*)buffer = info.type;
				*(shader_flags::flags*)(&buffer[shader_flags_index]) = (shader_flags::flags)flags;
				*(id::id_type*)(&buffer[argument_buffer_index]) = create_arguments_buffer(info.type, (shader_flags::flags)flags);
				*(u32*)(&buffer[texture_count_index]) = info.texture_count;

				initialize();

				if(info.texture_count)
				{
					 // TODO: 看看需要怎么处理 texture
					memcpy(_texture_ids, info.texture_ids, info.texture_count * sizeof(id::id_type));
					texture::get_descriptor_indices(_texture_ids, info.texture_count, _descriptor_inidices);
				}

				u32 shader_index{ 0 };
				for (u32 i{ 0 }; i < shader_type::count; ++i)
				{
					if (id::is_valid(info.shader_ids[i]))
					{
						_shader_ids[shader_index] = info.shader_ids[i];
						++shader_index;
					}
				}

				assert(shader_index == (u32)__builtin_popcount(_shader_flags));
			}

			[[nodiscard]] constexpr u32 texture_count() const { return _texture_count; }
			[[nodiscard]] constexpr material_type::type material_type() const { return _type; }
			[[nodiscard]] constexpr shader_flags::flags shader_flags() const { return _shader_flags; }
			[[nodiscard]] constexpr id::id_type arg_buffer_id() const { return _argument_buffer_id; }
			[[nodiscard]] constexpr id::id_type* texture_ids() const { return _texture_ids; }
			[[nodiscard]] constexpr u32* descriptor_indices() const { return _descriptor_inidices; }
			[[nodiscard]] constexpr id::id_type* shader_ids() const { return _shader_ids; }
		private:
			void initialize()
			{
				assert(_buffer);
				u8 *const buffer{ _buffer };

				_type = *(material_type::type*)buffer;
				_shader_flags = *(shader_flags::flags*)(&buffer[shader_flags_index]);
				_argument_buffer_id = *(id::id_type*)(&buffer[argument_buffer_index]);
				_texture_count = *(u32*)(&buffer[texture_count_index]);

				_shader_ids = (id::id_type*)(&buffer[texture_count_index + sizeof(u32)]);
				_texture_ids = _texture_count ? &_shader_ids[__builtin_popcount(_shader_flags)] : nullptr;
				_descriptor_inidices = _texture_count ? (u32*)(&_texture_ids[_texture_count]) : nullptr;
			}

			constexpr static u32						shader_flags_index{ sizeof(material_type::type) };
			constexpr static u32						argument_buffer_index{ shader_flags_index + sizeof(shader_flags::flags) };
			constexpr static u32						texture_count_index{ argument_buffer_index + sizeof(id::id_type) };

			u8*											_buffer;
			id::id_type*								_texture_ids;
			id::id_type*								_shader_ids;
			id::id_type									_argument_buffer_id;
			u32*										_descriptor_inidices;
			u32											_texture_count;
			material_type::type							_type;
			shader_flags::flags							_shader_flags;
		};

		constexpr MTL::PrimitiveType get_metal_primitive_topology_type(primitive_topology::type type)
		{
			using namespace primal::content;
			assert(type < primitive_topology::count);
			switch (type)
			{
			case primitive_topology::point_list:			return MTL::PrimitiveTypePoint;
			case primitive_topology::line_list:			return MTL::PrimitiveTypeLine;
			case primitive_topology::line_strip:			return MTL::PrimitiveTypeLineStrip;
			case primitive_topology::triangle_list:		return MTL::PrimitiveTypeTriangle;
			case primitive_topology::triangle_strip:		return MTL::PrimitiveTypeTriangleStrip;
			}

			return MTL::PrimitiveTypePoint;
		}

		id::id_type create_arguments_buffer(material_type::type type, shader_flags::flags flags)
		{
			assert(type < material_type::count);
			static_assert(sizeof(type) == sizeof(u32) && sizeof(flags) == sizeof(u32));
			const u64 key{ ((u64)type << 32) | flags };
			auto pair = mtl_rs_map.find(key);
			if (pair != mtl_rs_map.end())
			{
				assert(pair->first == key);
				return pair->second;
			}

			NS::Array* argArray{ nullptr };

			switch(type)
			{
			case primal::graphics::material_type::opauqe:
			{
				// TODO: Add shader use resources
				using params = gpass::opaque_root_parameter;

				// MTL::ResourceUsage buffer_visibility{ MTL::ResourceUsageRead };
				// MTL::ResourceUsage data_visibility{ MTL::ResourceUsageRead };

				MTL::ArgumentDescriptor* global_data_arg_desc{MTL::ArgumentDescriptor::alloc()->init()};
				global_data_arg_desc->setIndex(params::global_shader_data);
				global_data_arg_desc->setDataType(MTL::DataTypePointer);
				global_data_arg_desc->setAccess(MTL::ArgumentAccessReadOnly);

				MTL::ArgumentDescriptor* per_object_data_arg_desc{MTL::ArgumentDescriptor::alloc()->init()};
				per_object_data_arg_desc->setIndex(params::per_object_data);
				per_object_data_arg_desc->setDataType(MTL::DataTypePointer);
				per_object_data_arg_desc->setAccess(MTL::ArgumentAccessReadOnly);

				MTL::ArgumentDescriptor* position_buffer_arg_desc{MTL::ArgumentDescriptor::alloc()->init()};
				position_buffer_arg_desc->setIndex(params::position_buffer);
				position_buffer_arg_desc->setDataType(MTL::DataTypePointer);
				position_buffer_arg_desc->setAccess(MTL::ArgumentAccessReadOnly);

				MTL::ArgumentDescriptor* element_buffer_arg_desc{MTL::ArgumentDescriptor::alloc()->init()};
				element_buffer_arg_desc->setIndex(params::element_buffer);
				element_buffer_arg_desc->setDataType(MTL::DataTypePointer);
				element_buffer_arg_desc->setAccess(MTL::ArgumentAccessReadOnly);

				MTL::ArgumentDescriptor* srv_indices_arg_desc{MTL::ArgumentDescriptor::alloc()->init()};
				srv_indices_arg_desc->setIndex(params::srv_indices);
				srv_indices_arg_desc->setDataType(MTL::DataTypePointer);
				srv_indices_arg_desc->setAccess(MTL::ArgumentAccessReadOnly);

				MTL::ArgumentDescriptor* directional_light_arg_desc{MTL::ArgumentDescriptor::alloc()->init()};
				directional_light_arg_desc->setIndex(params::directional_lights);
				directional_light_arg_desc->setDataType(MTL::DataTypePointer);
				directional_light_arg_desc->setAccess(MTL::ArgumentAccessReadOnly);

				NS::Object* descs[6]{ global_data_arg_desc, per_object_data_arg_desc, position_buffer_arg_desc, element_buffer_arg_desc, srv_indices_arg_desc, directional_light_arg_desc };
				argArray = NS::Array::array(descs, 6);
				
				argArray->retain();
			}
			break;
			default: break;
			}

			assert(argArray);
			const id::id_type id{ (id::id_type)argument_buffer_layouts.size() };
			argument_buffer_layouts.emplace_back(argArray);
			mtl_rs_map[key] = id;

			return id;
		}

		id::id_type create_pso_if_needed(const u8* const stream_ptr, u64 align_stream_size, [[maybe_unused]] bool is_depth)
		{
			const u64 key{ math::calc_crc32_u64(stream_ptr, align_stream_size) };
			auto pair = pso_map.find(key);

			if (pair != pso_map.end())
			{
				assert(pair->first == key);
				return  pair->second;
			}

			const id::id_type id{ (u32)pipeline_states.size() };
			
			metal_pipeline_state_stream* const stream{ (metal_pipeline_state_stream *const)stream_ptr };
			MTL::RenderPipelineState* pso{ create_pipeline_status(stream) };
			pipeline_states.emplace_back(pso);
			NAME_METAL_OBJECT_INDEXED(pipeline_states.back(), key,
				is_depth ? "Depth-only Pipeline State Object - key" : "GPASS Pipeline State Object - key");
			assert(id::is_valid(id));
			pso_map[key] = id;
			return id;
		}

		shader_type::type get_shader_type(u32 flag)
		{
			assert(flag);
			unsigned long index = __builtin_ctz(flag);
			return (shader_type::type)index;
		}

		pso_id create_pso(id::id_type material_id, MTL::PrimitiveType primitive_topology, u32 elements_type)
		{
			std::lock_guard lock{ material_mutex };
			const metal_material_stream material{ materials[material_id].get() };

			constexpr u64 aligned_stream_size{ math::align_size_up<sizeof(u64)>(sizeof(metal_pipeline_state_stream)) };
			u8 *const stream_ptr{ (u8 *const)alloca(aligned_stream_size) };
			memset(stream_ptr, 0, aligned_stream_size);
			new (stream_ptr) metal_pipeline_state_stream{};

			metal_pipeline_state_stream& stream{ *(metal_pipeline_state_stream *const)stream_ptr };

			MTL::RenderPipelineColorAttachmentDescriptor* color_attachment{ MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init() };
			color_attachment->setPixelFormat(gpass::main_buffer_format);
			color_attachment->setBlendingEnabled(true);
			color_attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
			color_attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
			color_attachment->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
			// Create normal depth attachment for SSAO
			MTL::RenderPipelineColorAttachmentDescriptor* normal_depth_attachment{ MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init() };
			normal_depth_attachment->setPixelFormat(gpass::main_buffer_format);
			normal_depth_attachment->setBlendingEnabled(true);
			normal_depth_attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
			normal_depth_attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
			normal_depth_attachment->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
			// Create albedo attachment for SSDO
			MTL::RenderPipelineColorAttachmentDescriptor* albedo_attachment{ MTL::RenderPipelineColorAttachmentDescriptor::alloc()->init() };
			albedo_attachment->setPixelFormat(gpass::main_buffer_format);
			albedo_attachment->setBlendingEnabled(true);
			albedo_attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
			albedo_attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
			albedo_attachment->setSourceAlphaBlendFactor(MTL::BlendFactorSourceAlpha);
			
			METAL_COLOR_ATTACHMENT_ARRAY color_attachments{};
			color_attachments.descs[0] = color_attachment;
			color_attachments.descs[1] = normal_depth_attachment;
			color_attachments.descs[2] = albedo_attachment;
			color_attachments.count = 3;
			
			stream.color_attachments = color_attachments;
			stream.depth_attachment_format = gpass::depth_buffer_format;
			stream.primitive_topology = primitive_topology;
			stream.rasterizer_state = true;
			stream.sample_count = 1;
			stream.alpha_to_coverage = false;

			const shader_flags::flags flags{ material.shader_flags() };
			MTL::Function* shaders[shader_type::count]{};
			u32 shader_index{ 0 };

			for (u32 i{ 0 }; i < shader_type::count; ++i)
			{
				if (flags & (1 << i))
				{
					// 注意：每种类型的着色器可能根据子网格或材质的不同属性生成不同的键
					// 目前，我们只有根据 element_type 不同的顶点着色器
					const u32 key{ get_shader_type(flags & (1 << i)) == shader_type::vertex ? elements_type : u32_invalid_id };
					primal::content::compiled_shader_ptr shader{ primal::content::get_shader(material.shader_ids()[shader_index], key) };
					assert(shader->byte_code() && shader->byte_code_size() != 0);

					// 创建 dispatch_data_t
					dispatch_data_t data = dispatch_data_create(
						shader->byte_code(),    // 数据指针
						shader->byte_code_size(),    // 数据长度
						nullptr,          // 队列（使用默认队列）
						^{
							// 释放回调（可选）
							// 这里 buffer 是 vector 管理的，dispatch_data_t 不会持有 buffer.data()，
							// 所以无需额外释放
						}
					);
					if (!data) {
						// 处理 dispatch_data_t 创建失败
						return { id::invalid_id, id::invalid_id };
					}
					NS::Error* error{ nullptr };

					// 创建 Metal 着色器函数
					id::id_type shader_id{ material.shader_ids()[shader_index] };
					NS::String* function_name{ NS::String::string(primal::content::get_shader_function_name(shader_id), NS::UTF8StringEncoding) };
					MTL::Library* library{ core::get_device()->newLibrary(data, &error) };
					if (library) {
						shaders[i] = library->newFunction(function_name);
						library->release();
					}
					else {
						std::cerr << error->localizedDescription()->utf8String() << std::endl;
					}

					++shader_index;
				}
			}

			// 设置着色器
			stream.vertex_function = shaders[shader_type::vertex];
			stream.fragment_function = shaders[shader_type::pixel];
			// stream.tessellation_function = shaders[shader_type::hull]; // Metal 中曲面细分着色器对应 D3D12 的 hull shader
			stream.compute_function = shaders[shader_type::compute];

			pso_id id_pair{};
			id_pair.gpass_pso_id = create_pso_if_needed(stream_ptr, aligned_stream_size, false);

			METAL_COLOR_ATTACHMENT_ARRAY color_attachments_depth{};
			color_attachments_depth.descs[0] = nullptr;
			color_attachments_depth.count = 0;
			stream.color_attachments = color_attachments_depth;
			stream.fragment_function = nullptr;
			// stream.input_primitive_topology = MTL::PrimitiveTopologyClassTriangle;
			id_pair.depth_pso_id = create_pso_if_needed(stream_ptr, aligned_stream_size, true);

			// 释放资源
			// color_attachments->release();
			for (u32 i{ 0 }; i < shader_type::count; ++i)
			{
				if (shaders[i]) shaders[i]->release();
			}

			return id_pair;
		}

		// NOTE: expects data to contain
		// struct {
		//         u32 width, height, array_size(or depth), flags, mip_levels, format,
		//         struct{
		//             u32 width, height, row_pitch, slice_pitch,
		//             u8 image[slice_pitch],
		//         } images[]
		// } texture
		metal_texture create_resource_from_texture_data(const u8* const data)
		{
			assert(data);
			utl::blob_stream_reader blob{ data };
			const u32 width{ blob.read<u32>() };
			const u32 height{ blob.read<u32>() };
			u32 depth{ 1 };
			u32 array_size{ blob.read<u32>() };
			const u32 flags{ blob.read<u32>() };
			const u32 mip_levels{ blob.read<u32>() };
			const MTL::PixelFormat format{ (MTL::PixelFormat)blob.read<u32>() };
			const bool is_3d{ (flags & primal::content::texture_flags::is_volume_map) != 0 };

			assert(mip_levels <= metal_texture::max_mips);
			u32 depth_per_mip_level[metal_texture::max_mips]{};
			for (u32 i{ 0 }; i < metal_texture::max_mips; ++i)
			{
				depth_per_mip_level[i] = 1;
			}

			if (is_3d)
			{
				depth = array_size;
				array_size = 1;
				u32 depth_per_mip{ depth };

				for (u32 i{ 0 }; i < mip_levels; ++i)
				{
					depth_per_mip_level[i] = depth_per_mip;
					depth_per_mip = std::max(depth_per_mip >> 1, (u32)1);
				}
			}

			MTL::TextureDescriptor* desc{ MTL::TextureDescriptor::alloc()->init() };
			desc->setWidth(width);
			desc->setHeight(height);
			desc->setDepth(depth);
			desc->setArrayLength(array_size);
			desc->setMipmapLevelCount(mip_levels);
			desc->setTextureType(is_3d ? MTL::TextureType3D : MTL::TextureType2D);
			// TODO: change Private and use upload context
			desc->setStorageMode(MTL::StorageModeManaged);
			desc->setPixelFormat(format);
			desc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);
			MTL::Texture* texture{ core::get_device()->newTexture(desc) };
			assert(texture);

			for (u32 i{ 0 }; i < array_size; ++i)
			{
				for (u32 j{ 0 }; j < mip_levels; ++j)
				{
					const u32 row_pitch{ blob.read<u32>() };
					const u32 slice_pitch{ blob.read<u32>() };

					texture->replaceRegion(MTL::Region{ 0, 0, 0, width, height, depth_per_mip_level[j] },
						j, blob.position(), row_pitch);

					blob.skip(slice_pitch * depth_per_mip_level[j]);

					// skip the rest of the slices of 3d textures with depth > 1
				}
			}
			metal_texture_init_info info{};
			info.resource = texture;

			return metal_texture{ info };
		}
    } // anonymous namespace

	bool initialize()
	{
		return true;
	}

	void shutdown()
	{
		// NOTE: we only release data that were created as a side-effect to adding resources,
		//		 which the user of this module has no control over. The rest of data should be released
		//		 by the user, by calling "remove" functions, prior to shutting down the renderer.
		//		 That way we make sure the book-keeping of content is correct.

		mtl_rs_map.clear();
		argument_buffer_layouts.clear();

		for (auto& item : pipeline_states)
		{
			core::release(item);
		}
		pso_map.clear();
		pipeline_states.clear();
	}

    namespace submesh
    {
    	// NOTE: Expects 'data' to contain:
		// u32 element_size, u32 vertex_count,
		// u32 index_count, u32 elements_type, u32 primitive_topology
		// u8 positions[sizeof(f32) * 3 * vertex_count], // sizeof(positions) must be a multiple of 16 bytes. Pad if needed.
		// u8 elements[sizeof(element_size) * vertex_count], // sizeof(elements) must be a multiple of 16 bytes. Pad if needed.
		// u8 indices[index_size * index_count],
		//
		// Remarks:
		// ~Advances the data pointer
		// ~Position and element buffers should be padded to be a multiple of 16 bytes in length.
		// This is 16byte is defined as 4
		id::id_type add(const u8*& data)
		{
			utl::blob_stream_reader blob{ (const u8*)data };

			const u32 element_size{ blob.read<u32>() };
			const u32 vertex_count{ blob.read<u32>() };
			const u32 index_count{ blob.read<u32>() };
			const u32 elements_type{ blob.read<u32>() };
			const u32 primitive_topology{ blob.read<u32>() };
			const u32 index_size{ static_cast<u32>((vertex_count < (1 << 16)) ? sizeof(u16) : sizeof(u32)) };

			// NOTE: element size may be 0, for position-only vertex formats.
			// TODO: remove this hard code about sizeof math::v3
			const u32 position_buffer_size{ static_cast<u32>(12 * vertex_count) };
			// sizeof(math::v3) = 16
			// const u32 position_buffer_size{ static_cast<u32>(sizeof(math::v3) * vertex_count) };
			const u32 element_buffer_size{ element_size * vertex_count };
			const u32 index_buffer_size{ index_size * index_count };

			constexpr u32 alignment{ 4 };
			const u32 aligned_position_buffer_size{ (u32)math::align_size_up<alignment>(position_buffer_size) };
			const u32 aligned_element_buffer_size{ (u32)math::align_size_up<alignment>(element_buffer_size) };
			const u32 total_buffer_size{ aligned_position_buffer_size + aligned_element_buffer_size + index_buffer_size };

			MTL::Device* device{ core::get_device() };
			MTL::Buffer* resource{ device->newBuffer(blob.position(), total_buffer_size, MTL::ResourceStorageModeShared) };
			// resource->didModifyRange(NS::Range{ 0, total_buffer_size });

			blob.skip(total_buffer_size);
			data = blob.position();

			submesh_view view{};
			view.position_buffer_view.offset = 0;
			view.position_buffer_view.size = position_buffer_size;
			view.position_buffer_view.stride = 12; //sizeof(math::v3); 

			if( element_size ) 
			{
				view.element_buffer_view.offset = aligned_position_buffer_size;
				view.element_buffer_view.size = element_buffer_size;
				view.element_buffer_view.stride = element_size;
			}

			view.index_buffer_view.offset = aligned_position_buffer_size + aligned_element_buffer_size;
			view.index_buffer_view.size = index_buffer_size;
			view.index_buffer_view.stride = index_size;
			
			view.elements_type = elements_type;
			view.primitive_topology = get_metal_primitive_topology_type((primitive_topology::type)primitive_topology);

			std::lock_guard lock{ submesh_mutex };
			submesh_buffers.add(resource);
			return submesh_views.add(view);
		}

		void remove(id::id_type id)
		{
			std::lock_guard lock{ submesh_mutex };
			submesh_views.remove(id);

			core::deferred_release(submesh_buffers[id]);
			submesh_buffers.remove(id);
		}

		void get_views(const id::id_type *const gpu_ids, u32 id_count, const views_cache& cache)
		{
			assert(gpu_ids && id_count);
			assert(cache.position_buffer_view && cache.element_buffer_view && cache.index_buffer_view && cache.primitive_topologies && cache.elements_types);

			std::lock_guard lock{ submesh_mutex };
			for (u32 i{ 0 }; i < id_count; ++i)
			{
				const submesh_view& view{ submesh_views[gpu_ids[i]] };
				cache.position_buffer_view[i] = view.position_buffer_view;
				cache.element_buffer_view[i] = view.element_buffer_view;
				cache.index_buffer_view[i] = view.index_buffer_view;
				cache.primitive_topologies[i] = view.primitive_topology;
				cache.elements_types[i] = view.elements_type;
				cache.view_buffer[i] = submesh_buffers[gpu_ids[i]];
			}
		}
    } // submesh namespace

    namespace texture
    {
		utl::vector<MTL::Texture*> get_texture_array()
		{
			utl::vector<MTL::Texture*> texture_array;
			for(u32 i { 0 }; i < textures.size(); ++i)
			{
				texture_array.push_back(textures[i].texture());
			}
			return texture_array;
		}

		// NOTE: expects data to contain
		// struct {
		//         u32 width, height, array_size(or depth), flags, mip_levels, format,
		//         struct{
		//             u32 width, height, row_pitch, slice_pitch,
		//             u8 image[slice_pitch],
		//         } images[]
		// } texture

		id::id_type add(const u8* const data)
		{
			assert(data);
			metal_texture texture{ create_resource_from_texture_data(data) };

			std::lock_guard lock{ texture_mutex };
			const id::id_type id{ textures.add(std::move(texture)) };
			descriptor_indices.add(id);
			assert(id::is_valid(id));
			return id;
		}

		void remove(id::id_type id)
		{
			std::lock_guard lock{ texture_mutex };
			textures.remove(id);
			descriptor_indices.remove(id);
		}

		void get_descriptor_indices(const id::id_type *const texture_ids, u32 id_count, u32 *const indices)
		{
			assert(texture_ids && id_count && indices);
			std::lock_guard lock{ texture_mutex };
			for (u32 i{ 0 }; i < id_count; ++i)
			{
				indices[i] = descriptor_indices[texture_ids[i]];
			}
		}

    } // texture namespace

    namespace material
    {
		// Output Format
		//
		// struct{
		//	material_type::type	type,
		//	shader_flags::flags	flags,
		//	id::id_type			root_signature_id,
		//	u32					texture_count,
		//	id::id_type			shader_ids[shader_count],
		//	id::id_type*		texture_ids[texture_count],
		// } metal_material;
		id::id_type add(material_init_info info)
		{
			std::unique_ptr<u8[]> buffer;
			std::lock_guard lock{ material_mutex };
			metal_material_stream stream{ buffer, info };
			assert(buffer);
			return materials.add(std::move(buffer));
		}

		void remove(id::id_type id)
		{
			std::lock_guard lock{ material_mutex };
			materials.remove(id);
		}

		void get_materials(const id::id_type *const material_ids, u32 material_count, const materials_cache& cache, u32& descriptor_index_count)
		{
			assert(material_ids && material_count);
			assert(cache.argument_buffer_layouts && cache.material_types);
			std::lock_guard lock{ material_mutex };

			u32 total_index_count{ 0 };
			for (u32 i{ 0 }; i < material_count; ++i)
			{
				const metal_material_stream stream{ materials[material_ids[i]].get() };
				cache.argument_buffer_layouts[i] = argument_buffer_layouts[stream.arg_buffer_id()];
				cache.material_types[i] = stream.material_type();
				cache.descriptor_indices[i] = stream.descriptor_indices();
				cache.texture_count[i] = stream.texture_count();

				total_index_count += stream.texture_count();
			}

			descriptor_index_count = total_index_count;
		}

    } // material namespace

    namespace render_item
    {
		// Creates a buffer that's basically an array of id::id_types.
		// buffer[0] = geometry_content_id
		// buffer[1 .. n] = metal_render_item_ids (n is the number of low-level render item ids which must also equal the number of submeshes/material ids)
		// buffer[n + 1] = id::invalid_id (this marks the end of submesh_gpu_id array)
		id::id_type add(id::id_type entity_id, id::id_type geometry_content_id, u32 material_count, const id::id_type *const material_ids)
		{
			assert(id::is_valid(entity_id) && id::is_valid(geometry_content_id));
			assert(material_count && material_ids);
			id::id_type *const gpu_ids{ (id::id_type *const)alloca(material_count * sizeof(id::id_type)) };
			primal::content::get_submesh_gpu_ids(geometry_content_id, material_count, gpu_ids);

			submesh::views_cache views_cache
			{
				(MTL::Buffer **const)alloca(sizeof(MTL::Buffer*) * material_count),
				(buffer_view *const)alloca(sizeof(buffer_view) * material_count),
				(buffer_view *const)alloca(sizeof(buffer_view) * material_count),
				(buffer_view *const)alloca(sizeof(buffer_view) * material_count),
				(MTL::PrimitiveType *const)alloca(sizeof(MTL::PrimitiveType) * material_count),
				(u32 *const)alloca(sizeof(u32) * material_count)
			};

			submesh::get_views(gpu_ids, material_count, views_cache);

			// NOTE: the list of ids starts with geometry id and ends with an invalid id to mark the end of the list.
			std::unique_ptr<id::id_type[]> items{ std::make_unique<id::id_type[]>(sizeof(id::id_type) * (1 + (u64)material_count + 1)) };

			items[0] = geometry_content_id;
			id::id_type *const item_ids{ &items[1] };

			std::lock_guard lock{ render_item_mutex };

			for(u32 i{ 0 }; i < material_count; ++i)
			{
				metal_render_item item{};
				item.entity_id = entity_id;
				item.submesh_gpu_id = gpu_ids[i];
				item.material_id = material_ids[i];
				pso_id id_pair{ create_pso(item.material_id, views_cache.primitive_topologies[i], views_cache.elements_types[i]) };
				item.pso_id = id_pair.gpass_pso_id;
				item.depth_pso_id = id_pair.depth_pso_id;

				assert(id::is_valid(item.submesh_gpu_id) && id::is_valid(item.material_id));
				item_ids[i] = render_items.add(item);
			}

			item_ids[material_count] = id::invalid_id;

			return render_item_ids.add(std::move(items));
		}

		void remove(id::id_type id)
		{
			std::lock_guard lock{ render_item_mutex };
			const id::id_type *const item_ids{ &render_item_ids[id][1] };

			// NOTE: the last element in the list of ids is always an invalid id.
			for (u32 i{ 0 }; item_ids[i] != id::invalid_id; ++i)
			{
				render_items.remove(item_ids[i]);
			}

			render_item_ids.remove(id);
		}

		void get_metal_render_item_ids(const frame_info& info, utl::vector<id::id_type>& metal_render_item_ids)
		{
			assert(info.render_item_ids && info.thresholds && info.render_item_count);
			assert(metal_render_item_ids.empty());

			frame_cache.lod_offsets.clear();
			frame_cache.geometry_ids.clear();
			const u32 count{ info.render_item_count };

			std::lock_guard lock{ render_item_mutex };

			for (u32 i{ 0 }; i < count; ++i)
			{
				const id::id_type *const buffer{ render_item_ids[info.render_item_ids[i]].get() };
				frame_cache.geometry_ids.emplace_back(buffer[0]);
			}

			primal::content::get_lod_offsets(frame_cache.geometry_ids.data(), info.thresholds, count, frame_cache.lod_offsets);

			assert(frame_cache.lod_offsets.size() == count);

			u32 metal_render_item_count{ 0 };
			for (u32 i{ 0 }; i < count; ++i)
			{
				metal_render_item_count += frame_cache.lod_offsets[i].count;; // frame_cache.lod_offsets[i].count;
			}

			assert(metal_render_item_count);
			metal_render_item_ids.resize(metal_render_item_count);

			u32 item_index{ 0 };
			for (u32 i{ 0 }; i < count; ++i)
			{
				const id::id_type *const item_ids{ &render_item_ids[info.render_item_ids[i]][1] };
				const primal::content::lod_offset& lod_offset{ frame_cache.lod_offsets[i] };
				memcpy(&metal_render_item_ids[item_index], &item_ids[lod_offset.offset], sizeof(id::id_type) * lod_offset.count);
				item_index += lod_offset.count;; // lod_offset.count;
				assert(item_index <= metal_render_item_count);
			}

			assert(item_index <= metal_render_item_count);
		}

		void get_items(const id::id_type *const metal_render_item_ids, u32 id_count, const items_cache& cache)
		{
			assert(metal_render_item_ids && id_count);
			assert(cache.entity_ids && cache.submesh_gpu_ids && cache.material_ids && cache.gpass_psos && cache.depth_psos);

			std::lock_guard lock{ render_item_mutex };

			for (u32 i{ 0 }; i < id_count; ++i)
			{
				const metal_render_item& item{ render_items[metal_render_item_ids[i]] };
				cache.entity_ids[i] = item.entity_id;
				cache.submesh_gpu_ids[i] = item.submesh_gpu_id;
				cache.material_ids[i] = item.material_id;
				cache.gpass_psos[i] = pipeline_states[item.pso_id];
				cache.depth_psos[i] = pipeline_states[item.depth_pso_id];
			}
		}
    } // render_item namespace
}
