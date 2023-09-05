#include "VulkanLight.h"
#include "Graphics/Renderer.h"
#include "EngineAPI/Light.h"
#include "EngineAPI/GameEntity.h"
#include "VulkanUtility.h"
#include "VulkanContent.h"

namespace primal::graphics::vulkan::light
{
	namespace 
	{
		struct light_owner
		{
			game_entity::entity_id			id{ id::invalid_id };
			u32								data_index;
			graphics::light::type			type;
			bool							is_enabled;
		};

		struct light_data
		{
			math::v3						position;
			math::v3						direction;
			math::m4x4						affine;
			math::m4x4						mvp;
			f32								intensity;
			math::v3						color;
			// input
			// utl::vector<id::id_type>		input_tex;
			f32								falloff;
			// output
			// for RSM : depth, position, normal, flux(this is the reflecter of the scene, p.s. this is diffuse shading)
			utl::vector<id::id_type>		output_tex;
		};

#if USE_STL_VECTOR
#define CONSTEXPR
#else
#define CONSTEXPR constexpr
#endif

		class light_set
		{
		public:
			constexpr graphics::light add(light_init_info info)
			{
				if (info.type == graphics::light::directional)
				{
					u32 index{ id::invalid_id };
					for (u32 i{ 0 }; i < _non_cullable_owners.size(); ++i)
					{
						if (!id::is_valid(_non_cullable_owners[i]))
						{
							index = i;
							break;
						}
					}

					if (index == id::invalid_id)
					{
						index = (u32)_non_cullable_owners.size();
						_non_cullable_owners.emplace_back();
						_non_cullable_lights.emplace_back();
					}

					light_data& data{ _non_cullable_lights[index] };
					data.color = info.color;
					data.intensity = info.intensity;
					data.falloff = 0.f;
					for (u32 i{ 0 }; i < 4; ++i)
					{
						data.output_tex.emplace_back(create_output_tex());
					}
					
					light_owner owner{ game_entity::entity_id{info.entity_id}, index, info.type, info.is_enabled };
					const light_id id{ _owners.add(owner) };
					_non_cullable_owners[index] = id;

					return graphics::light{ id, info.light_set_key };
				}
				else
				{
					return {};
				}
			}

			constexpr void remove(light_id id)
			{
				enable(id, false);

				const light_owner& owner{ _owners[id] };

				if (owner.type == graphics::light::directional)
				{
					_non_cullable_owners[owner.data_index] = light_id{ id::invalid_id };
				}
				else
				{
					// TODO: cullable lights
				}

				_owners.remove(id);
			}

			void update_transforms()
			{
				// Update direction for non_cullable_lights
				for (const auto& id : _non_cullable_owners)
				{
					if (!id::is_valid(id)) continue;

					const light_owner& owner{ _owners[id] };
					if (owner.is_enabled)
					{
						const game_entity::entity entity{ game_entity::entity_id{ owner.id } };
						light_data& params{ _non_cullable_lights[owner.data_index] };
						params.position = entity.position();
						params.direction = entity.orientation();
						using namespace DirectX;
						XMVECTOR translate{ entity.position().x, entity.position().y, entity.position().z };
						XMVECTOR rotate{ entity.rotation().x, entity.rotation().y, entity.rotation().z, entity.rotation().w };
						XMVECTOR scale{ entity.scale().x, entity.scale().y, entity.scale().z };
						XMMATRIX aff = XMMatrixAffineTransformation(scale, { 0, 0, 0 }, rotate, translate);
						XMStoreFloat4x4(&params.affine, aff);
						XMMATRIX model = XMMatrixIdentity();
						XMMATRIX view = XMMatrixLookAtRH(translate, { entity.orientation().x , entity.orientation().y, entity.orientation().z }, { 0, 1, 0 });
						XMMATRIX projection = XMMatrixOrthographicRH(2048, 2048, 0.1f, 64.f);
						XMStoreFloat4x4(&params.mvp, XMMatrixMultiply(projection, XMMatrixMultiply(view, model)));
					}
				}

				// TODO: cullable lights
			}

			constexpr void enable(light_id id, bool is_enabled)
			{
				_owners[id].is_enabled = is_enabled;

				if (_owners[id].type == graphics::light::directional)
				{
					return;
				}

				// TODO: cullable lights
			}

			constexpr void intensity(light_id id, f32 intensity)
			{
				if (intensity < 0.f) intensity = 0.f;

				const light_owner& owner{ _owners[id] };
				const u32 index{ owner.data_index };

				if (_owners[id].type == graphics::light::directional)
				{
					assert(index < _non_cullable_lights.size());
					_non_cullable_lights[index].intensity = intensity;
				}
				else
				{
					// TODO: cullable lights
				}
			}

			constexpr void color(light_id id, math::v3 color)
			{
				assert(color.x <= 1.f && color.y <= 1.f && color.z <= 1.f);
				assert(color.x >= 0.f && color.y >= 0.f && color.z >= 0.f);

				const light_owner& owner{ _owners[id] };
				const u32 index{ owner.data_index };

				if (_owners[id].type == graphics::light::directional)
				{
					assert(index < _non_cullable_lights.size());
					_non_cullable_lights[index].color = color;
				}
				else
				{
					// TODO: cullable lights
				}
			}

			constexpr void color(light_id id, f32 falloff)
			{
				assert(falloff);

				const light_owner& owner{ _owners[id] };
				const u32 index{ owner.data_index };

				if (_owners[id].type == graphics::light::directional)
				{
					assert(index < _non_cullable_lights.size());
					_non_cullable_lights[index].falloff = falloff;
				}
				else
				{
					// TODO: cullable lights
				}
			}

			constexpr bool is_enabled(light_id id) const
			{
				return _owners[id].is_enabled;
			}

			constexpr f32 intensity(light_id id) const
			{
				const light_owner& owner{ _owners[id] };
				const u32 index{ owner.data_index };

				if (_owners[id].type == graphics::light::directional)
				{
					assert(index < _non_cullable_lights.size());
					return _non_cullable_lights[index].intensity;
				}
				// TODO: cullable lights

				return 0.f;
			}

			constexpr math::v3 color(light_id id)
			{
				const light_owner& owner{ _owners[id] };
				const u32 index{ owner.data_index };

				if (_owners[id].type == graphics::light::directional)
				{
					assert(index < _non_cullable_lights.size());
					return _non_cullable_lights[index].color;
				}
				// TODO: cullable lights

				return {};
			}

			constexpr f32 falloff(light_id id)
			{
				const light_owner& owner{ _owners[id] };
				const u32 index{ owner.data_index };

				if (_owners[id].type == graphics::light::directional)
				{
					assert(index < _non_cullable_lights.size());
					return _non_cullable_lights[index].falloff;
				}
				// TODO: cullable lights

				return {};
			}

			utl::vector<id::id_type> output_tex(light_id id)
			{
				const light_owner& owner{ _owners[id] };
				const u32 index{ owner.data_index };

				if (_owners[id].type == graphics::light::directional)
				{
					assert(index < _non_cullable_lights.size());
					return _non_cullable_lights[index].output_tex;
				}
				// TODO: cullable lights

				return {};
			}

			constexpr graphics::light::type type(light_id id)
			{
				return _owners[id].type;
			}

			constexpr id::id_type entity_id(light_id id)
			{
				return _owners[id].id;
			}

			// Return the nunber of enabled directional lights
			CONSTEXPR u32 non_cullable_light_count() const
			{
				u32 count{ 0 };
				for (const auto& id : _non_cullable_owners)
				{
					if (id::is_valid(id) && _owners[id].is_enabled) ++count;
				}

				return count;
			}

			CONSTEXPR void non_cullable_lights(light_data* const lights, [[maybe_unused]] u32 buffer_size)
			{
				//assert(buffer_size == math::align_size_up<D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT>(non_cullable_light_count() * sizeof(light_data)));
				const u32 count{ (u32)_non_cullable_owners.size() };
				u32 index{ 0 };
				for (u32 i{ 0 }; i < count; ++i)
				{
					if (!id::is_valid(_non_cullable_owners[i])) continue;

					const light_owner& owner{ _owners[_non_cullable_owners[i]] };
					if (owner.is_enabled)
					{
						assert(_owners[_non_cullable_owners[i]].data_index == i);
						lights[index] = _non_cullable_lights[i];
						++index;
					}
				}
			}

			constexpr bool has_lights() const
			{
				return _owners.size() > 0;
			}

			id::id_type create_output_tex()
			{
				PE_vk_image_info img_info;
				PE_vk_image_view_info view_info;
				PE_vk_image_sampler_info sampler_info;
				return textures::add(img_info.info, view_info.info, sampler_info.info);
			}

		private:
			utl::free_list<light_owner>		_owners;
			utl::vector<light_data>			_non_cullable_lights;
			utl::vector<light_id>			_non_cullable_owners;
		};

#undef CONSTEXPR

		std::unordered_map<u64, light_set>			light_sets;

		constexpr void set_is_enabled(light_set& set, light_id id, const void* const data, [[maybe_unused]] u32 size)
		{
			bool is_enabled{ *(bool*)data };
			assert(sizeof(is_enabled) == size);
			set.enable(id, is_enabled);
		}

		constexpr void set_intensity(light_set& set, light_id id, const void* const data, [[maybe_unused]] u32 size)
		{
			f32 intensity{ *(f32*)data };
			assert(sizeof(intensity) == size);
			set.intensity(id, intensity);
		}

		constexpr void set_color(light_set& set, light_id id, const void* const data, [[maybe_unused]] u32 size)
		{
			math::v3 color{ *(math::v3*)data };
			assert(sizeof(color) == size);
			set.color(id, color);
		}

		constexpr void set_falloff(light_set& set, light_id id, const void* const data, [[maybe_unused]] u32 size)
		{
			f32 falloff{ *(f32*)data };
			assert(sizeof(falloff) == size);
			set.intensity(id, falloff);
		}

		constexpr void get_is_enabled(light_set& set, light_id id, void* const data, [[maybe_unused]] u32 size)
		{
			bool* const is_enabled{ (bool* const)data };
			assert(sizeof(bool) == size);
			*is_enabled = set.is_enabled(id);
		}

		constexpr void get_intensity(light_set& set, light_id id, void* const data, [[maybe_unused]] u32 size)
		{
			f32* const intensity{ (f32* const)data };
			assert(sizeof(f32) == size);
			*intensity = set.intensity(id);
		}

		constexpr void get_color(light_set& set, light_id id, void* const data, [[maybe_unused]] u32 size)
		{
			math::v3* const color{ (math::v3* const)data };
			assert(sizeof(math::v3) == size);
			*color = set.color(id);
		}

		constexpr void get_type(light_set& set, light_id id, void* const data, [[maybe_unused]] u32 size)
		{
			graphics::light::type* const type{ (graphics::light::type* const)data };
			assert(sizeof(graphics::light::type) == size);
			*type = set.type(id);
		}

		constexpr void get_entity_id(light_set& set, light_id id, void* const data, [[maybe_unused]] u32 size)
		{
			id::id_type* const entity_id{ (id::id_type* const)data };
			assert(sizeof(id::id_type) == size);
			*entity_id = set.entity_id(id);
		}

		constexpr void get_output_tex(light_set& set, light_id id, void* const data, [[maybe_unused]] u32 size)
		{
			id::id_type* const entity_id{ (id::id_type* const)data };
			assert(sizeof(id::id_type) == size);
			*entity_id = *set.output_tex(id).data();
		}

		constexpr void dummy_set(light_set&, light_id, const void* const, u32) {}

		using set_function = void(*)(light_set&, light_id, const void* const, u32);
		using get_function = void(*)(light_set&, light_id, void* const, u32);
		constexpr set_function set_functions[]
		{
			set_is_enabled,
			set_intensity,
			set_color,
			dummy_set,
			dummy_set
		};

		static_assert(_countof(set_functions) == light_parameter::count);

		constexpr get_function get_functions[]
		{
			get_is_enabled,
			get_intensity,
			get_color,
			get_type,
			get_entity_id
		};

		static_assert(_countof(get_functions) == light_parameter::count);
	} // anonymous namespace

	graphics::light create(light_init_info info)
	{
		assert(id::is_valid(info.entity_id));
		return light_sets[info.light_set_key].add(info);
	}

	void remove(light_id id, u64 light_set_key)
	{
		assert(light_sets.count(light_set_key));
		light_sets[light_set_key].remove(id);
	}

	void set_parameter(light_id id, u64 light_set_key, light_parameter::parameter parameter, const void* const data, u32 data_size)
	{
		assert(data && data_size);
		assert(light_sets.count(light_set_key));
		assert(parameter < light_parameter::count && set_functions[parameter] != dummy_set);
		set_functions[parameter](light_sets[light_set_key], id, data, data_size);
	}

	void get_parameter(light_id id, u64 light_set_key, light_parameter::parameter parameter, void* const data, u32 data_size)
	{
		assert(data && data_size);
		assert(light_sets.count(light_set_key));
		assert(parameter < light_parameter::count);
		get_functions[parameter](light_sets[light_set_key], id, data, data_size);
	}

	utl::vector<id::id_type> get_light_tex(light_id id, u64 light_set_key)
	{
		return light_sets[light_set_key].output_tex(id);
	}

	void update_light_buffers(const frame_info& info)
	{
		const u64 light_set_key{ info.light_set_key };
		assert(light_sets.count(light_set_key));
		light_set& set{ light_sets[light_set_key] };
		if (!set.has_lights()) return;

		set.update_transforms();
	}

	u32 non_cullable_light_count(u64 light_set_key)
	{
		assert(light_sets.count(light_set_key));
		return light_sets[light_set_key].non_cullable_light_count();
	}
}