#include "Common.h"
#include "CommonHeaders.h"
#include "Id.h"
#include "../Engine/Components/Entity.h"
#include "../Engine/Components/Transform.h"

using namespace primal;

namespace {
	struct transform_component
	{
		f32 position[3];
		f32 rotation[3];
		f32 scale[3];

		transform::init_info to_init_info()
		{
#if defined(_MSC_VER)
			using namespace DirectX;
			transform::init_info info{};
			memcpy(&info.position[0], &position[0], sizeof(position));
			memcpy(&info.scale[0], &scale[0], sizeof(scale));
			XMFLOAT3A rot{ &rotation[0] };
			XMVECTOR quat{ XMQuaternionRotationRollPitchYawFromVector(XMLoadFloat3A(&rot)) };
			XMFLOAT4A rot_quat{};
			XMStoreFloat4A(&rot_quat, quat);
			memcpy(&info.rotation, &rot_quat.x, sizeof(info.rotation));
			return info;
#elif defined(__clang__)
			transform::init_info info{};
			memcpy(&info.position[0], &position[0], sizeof(position));
			memcpy(&info.scale[0], &scale[0], sizeof(scale));
			
			math::v3 euler_angle{ rotation[0], rotation[1], rotation[2] };
			Eigen::Quaternionf quat{
				Eigen::AngleAxisf(euler_angle.x(), math::v3::UnitX()) * 
				Eigen::AngleAxisf(euler_angle.y(), math::v3::UnitY()) *
				Eigen::AngleAxisf(euler_angle.z(), math::v3::UnitZ())
			};

			info.rotation[0] = quat.x();
			info.rotation[1] = quat.y();
			info.rotation[2] = quat.z();
			info.rotation[3] = quat.w();

			return info;
#endif
		}
	};

	struct game_entity_descriptor
	{
		transform_component transform;
	};


	[[maybe_unused]] game_entity::entity entity_from_id(id::id_type id)
	{
		return game_entity::entity{ game_entity::entity_id(id) };
	}
}

EDITOR_INTERFACE id::id_type CreateGameEntity(game_entity_descriptor* e)
{
	assert(e);
	game_entity_descriptor& desc{ *e };
	transform::init_info transform_info{ desc.transform.to_init_info() };
	game_entity::entity_info entity_info
	{
		&transform_info,
	};
	return game_entity::create(entity_info).get_id();
}

EDITOR_INTERFACE void RemoveGameEntity(id::id_type id)
{
	assert(id::is_valid(id));
	game_entity::remove(game_entity::entity_id{ id });
}