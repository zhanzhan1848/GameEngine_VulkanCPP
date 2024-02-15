#include "EngineAPI/GameEntity.h"
#include "EngineAPI/Light.h"
#include "EngineAPI/TransformComponent.h"
#include "Graphics/Renderer.h"

#define RANDOM_LIGHTS 1

using namespace primal;

game_entity::entity create_one_game_entity(math::v3 position, math::v3 rotation, const char* name);
void remove_game_entity(game_entity::entity_id id);

namespace
{
	const u64 left_set{ 0 };
	const u64 right_set{ 1 };
	constexpr f32 inv_rand_max{ 1.f / RAND_MAX };

	utl::vector<graphics::light>	lights;

	constexpr math::v3 rgb_to_color(u8 r, u8 g, u8 b) { return { r / 255.f, g / 255.f, b / 255.f }; }
	f32 random(f32 min = 0.f) { return std::max(min, rand() * inv_rand_max); }

	void create_light(math::v3 position, math::v3 rotation, graphics::light::type type, u64 light_set_key)
	{
		game_entity::entity_id entity_id{ create_one_game_entity(position, rotation, nullptr).get_id() };

		graphics::light_init_info info{};
		info.entity_id = entity_id;
		info.type = type;
		info.light_set_key = light_set_key;
		info.intensity = 1.f;
		info.color = { random(0.2f), random(0.2f), random(0.2f) };

#if RANDOM_LIGHTS
		if (type == graphics::light::spot)
		{
			info.point_param.range = random(0.5f) * 0.2f;
			info.point_param.attenuation = { 1, 1, 1 };
		}
		else if (type == graphics::light::point)
		{
			info.spot_param.range = random(0.5f) * 0.2f;
			info.spot_param.umbra = (random(0.5f) - 0.4f) * math::pi;
			info.spot_param.penumbra = info.spot_param.umbra + (0.1f * math::pi);
			info.spot_param.attenuation = { 1, 1, 1 };
		}
#else
		if (type == graphics::light::spot)
		{
			info.point_param.range = 1.f;
			info.point_param.attenuation = { 1, 1, 1 };
		}
		else if (type == graphics::light::point)
		{
			info.spot_param.range = 2.f;
			info.spot_param.umbra = 0.7f * math::pi;
			info.spot_param.penumbra = info.spot_param.umbra + (0.1f * math::pi);
			info.spot_param.attenuation = { 1, 1, 1 };
		}
#endif
		graphics::light light{ graphics::create_light(info) };
		assert(light.is_valid());
		lights.push_back(light);
	}
} // anonymous namespace

void generate_lights()
{
	// LEFT_SET
	graphics::light_init_info info{};
	info.entity_id = create_one_game_entity({}, { 0, 0, 0 }, nullptr).get_id();
	info.type = graphics::light::directional;
	info.light_set_key = left_set;
	info.intensity = 1.f;
	info.color = rgb_to_color(174, 174, 174);

	lights.emplace_back(graphics::create_light(info));

	info.entity_id = create_one_game_entity({}, { math::pi * 0.5f, 0, 0 }, nullptr).get_id();
	info.color = rgb_to_color(17, 27, 48);
	lights.emplace_back(graphics::create_light(info));

	info.entity_id = create_one_game_entity({}, { -math::pi * 0.5f, 0, 0 }, nullptr).get_id();
	info.color = rgb_to_color(63, 47, 30);
	lights.emplace_back(graphics::create_light(info));

	// RIGHT_SET
	info.entity_id = create_one_game_entity({}, { 0, 0, 0 }, nullptr).get_id();
	info.color = rgb_to_color(150, 100, 200);
	info.light_set_key = right_set;
	lights.emplace_back(graphics::create_light(info));

	info.entity_id = create_one_game_entity({}, { math::pi * 0.5f, 0, 0 }, nullptr).get_id();
	info.color = rgb_to_color(17, 27, 48);
	lights.emplace_back(graphics::create_light(info));

	info.entity_id = create_one_game_entity({}, { -math::pi * 0.5f, 0, 0 }, nullptr).get_id();
	info.color = rgb_to_color(163, 47, 30);
	lights.emplace_back(graphics::create_light(info));

#if !RANDOM_LIGHTS
	create_light({ 0, -3, 0 }, {}, graphics::light::point, left_set);
	create_light({ 0, 0.2f, 1.f }, {}, graphics::light::point, left_set);
	create_light({ 0, 3, 2.5f }, {}, graphics::light::point, left_set);
	create_light({ 0, 0.1f, 7 }, { 0, 3.14f, 0}, graphics::light::spot, left_set);
#else
	srand(37);

	constexpr f32 scale1{ 2 };
	constexpr math::v3 scale{ 1.f * scale1, 0.5f * scale1, 1.f * scale1 };
	constexpr s32 dim{ 13 };
	for(s32 x{-dim}; x < dim; ++x)
		for(s32 y{0}; y < 2 * dim; ++y)
			for (s32 z{ -dim }; z < dim; ++z)
			{
				create_light({ (f32)(x * scale.x), (f32)(y * scale.y), (f32)(z * scale.z) },
					{ 3.14f, random(), 0.f }, 
					random() > 0.5 ? graphics::light::spot : graphics::light::point, left_set);
				create_light({ (f32)(x * scale.x), (f32)(y * scale.y), (f32)(z * scale.z) },
					{ 3.14f, random(), 0.f },
					random() > 0.5 ? graphics::light::spot : graphics::light::point, right_set);
			}
#endif
}

void remove_lights()
{
	for (auto& light : lights)
	{
		const game_entity::entity_id id{ light.entity_id() };
		graphics::remove_light(light.get_id(), light.light_set_key());
		remove_game_entity(id);
	}
	lights.clear();
}