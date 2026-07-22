#include "Common.h"
#include "CommonHeaders.h"
#include "Id.h"
#include "../Engine/Components/Entity.h"
#include "../Engine/Components/Transform.h"
#include "../Engine/Components/Mesh.h"
#include "../Engine/Components/Script.h"

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

			// 使用simd库进行欧拉角到四元数的转换
			using namespace simd;

			// 创建旋转四元数（Roll-Pitch-Yaw顺序）
			simd::quatf quat_x = simd_quaternion(rotation[0], simd_make_float3(1.0f, 0.0f, 0.0f)); // Roll (X轴)
			simd::quatf quat_y = simd_quaternion(rotation[1], simd_make_float3(0.0f, 1.0f, 0.0f)); // Pitch (Y轴)
			simd::quatf quat_z = simd_quaternion(rotation[2], simd_make_float3(0.0f, 0.0f, 1.0f)); // Yaw (Z轴)

			// 组合旋转：Z * Y * X（与Eigen的顺序保持一致）
			simd::quatf quat = simd_mul(simd_mul(quat_z, quat_y), quat_x);

			info.rotation[0] = quat.vector.x;
			info.rotation[1] = quat.vector.y;
			info.rotation[2] = quat.vector.z;
			info.rotation[3] = quat.vector.w;

			return info;
#endif
		}
	};

	struct mesh_component
	{
		id::id_type geometry_content_id{ id::invalid_id };
		id::id_type material_ids[8]{};
		u32 material_count{ 0 };

		mesh::init_info to_init_info()
		{
			mesh::init_info info{};
			info.geometry_content_id = geometry_content_id;
			info.material_ids = material_ids;
			info.material_count = material_count;
			return info;
		}
	};

	struct game_entity_descriptor
	{
		transform_component transform;
		mesh_component mesh;
	};

	[[maybe_unused]] game_entity::entity entity_from_id(id::id_type id)
	{
		return game_entity::entity{ game_entity::entity_id(id) };
	}

	// Euler angles (pitch, yaw, roll) → quaternion
	math::v4 euler_to_quat(const f32* rotation)
	{
#if defined(_MSC_VER)
		using namespace DirectX;
		XMFLOAT3A rot{ rotation[0], rotation[1], rotation[2] };
		XMVECTOR quat{ XMQuaternionRotationRollPitchYawFromVector(XMLoadFloat3A(&rot)) };
		XMFLOAT4A result{};
		XMStoreFloat4A(&result, quat);
		return math::v4{ result.x, result.y, result.z, result.w };
#elif defined(__clang__)
		using namespace simd;
		simd::quatf quat_x = simd_quaternion(rotation[0], simd_make_float3(1.0f, 0.0f, 0.0f));
		simd::quatf quat_y = simd_quaternion(rotation[1], simd_make_float3(0.0f, 1.0f, 0.0f));
		simd::quatf quat_z = simd_quaternion(rotation[2], simd_make_float3(0.0f, 0.0f, 1.0f));
		simd::quatf quat = simd_mul(simd_mul(quat_z, quat_y), quat_x);
		return math::v4{ quat.vector.x, quat.vector.y, quat.vector.z, quat.vector.w };
#endif
	}

	// Quaternion → Euler angles (pitch, yaw, roll)
	void quat_to_euler(math::v4 q, f32* out_rotation)
	{
		// Roll (X)
		f32 sinr_cosp = 2.0f * (q.w * q.x + q.y * q.z);
		f32 cosr_cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
		out_rotation[0] = atan2f(sinr_cosp, cosr_cosp);

		// Pitch (Y)
		f32 sinp = 2.0f * (q.w * q.y - q.z * q.x);
		sinp = sinp > 1.0f ? 1.0f : (sinp < -1.0f ? -1.0f : sinp);
		out_rotation[1] = asinf(sinp);

		// Yaw (Z)
		f32 siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
		f32 cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
		out_rotation[2] = atan2f(siny_cosp, cosy_cosp);
	}
}

// ============================================================================
// Entity Lifecycle
// ============================================================================

EDITOR_INTERFACE id::id_type CreateGameEntity(game_entity_descriptor* e)
{
	assert(e);
	game_entity_descriptor& desc{ *e };
	transform::init_info transform_info{ desc.transform.to_init_info() };
	mesh::init_info mesh_info{ desc.mesh.to_init_info() };

	game_entity::entity_info info{};
	info.transform = &transform_info;
	if (mesh_info.material_count > 0 && id::is_valid(mesh_info.geometry_content_id)) {
		info.mesh = &mesh_info;
	}

	return game_entity::create(info).get_id();
}

EDITOR_INTERFACE void RemoveGameEntity(id::id_type id)
{
	assert(id::is_valid(id));
	game_entity::remove(game_entity::entity_id{ id });
}

// ============================================================================
// Entity Query
// ============================================================================

EDITOR_INTERFACE u32 IsEntityAlive(id::id_type id)
{
	if (!id::is_valid(id)) return 0;
	return game_entity::is_alive(game_entity::entity_id{ id }) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GetEntityCount()
{
	return game_entity::entity_count();
}

// ============================================================================
// Transform
// ============================================================================

EDITOR_INTERFACE void GetEntityTransform(id::id_type id, f32* position, f32* rotation, f32* scale)
{
	if (!id::is_valid(id)) return;
	game_entity::entity entity{ entity_from_id(id) };
	if (!entity.is_valid()) return;

	if (position) {
		math::v3 pos{ entity.position() };
		position[0] = pos.x;
		position[1] = pos.y;
		position[2] = pos.z;
	}
	if (rotation) {
		quat_to_euler(entity.rotation(), rotation);
	}
	if (scale) {
		math::v3 scl{ entity.scale() };
		scale[0] = scl.x;
		scale[1] = scl.y;
		scale[2] = scl.z;
	}
}

EDITOR_INTERFACE void SetEntityTransform(id::id_type id, const f32* position, const f32* rotation, const f32* scale)
{
	if (!id::is_valid(id)) return;
	game_entity::entity entity{ entity_from_id(id) };
	if (!entity.is_valid()) return;

	transform::component_cache cache{};
	cache.id = transform::transform_id{ id };
	u32 flags = 0;

	if (position) {
		cache.position = math::v3{ position[0], position[1], position[2] };
		flags |= transform::component_flags::position;
	}
	if (rotation) {
		cache.rotation = euler_to_quat(rotation);
		flags |= transform::component_flags::rotation;
	}
	if (scale) {
		cache.scale = math::v3{ scale[0], scale[1], scale[2] };
		flags |= transform::component_flags::scale;
	}

	if (flags) {
		cache.flags = flags;
		transform::update(&cache, 1);
	}
}

// ============================================================================
// Mesh
// ============================================================================

EDITOR_INTERFACE id::id_type GetEntityMeshGeometryId(id::id_type id)
{
	if (!id::is_valid(id)) return id::invalid_id;
	game_entity::entity entity{ entity_from_id(id) };
	if (!entity.is_valid()) return id::invalid_id;
	mesh::component mc{ entity.mesh() };
	if (!mc.is_valid()) return id::invalid_id;
	return mesh::get_geometry_id(mc);
}

EDITOR_INTERFACE void SetEntityMesh(id::id_type id, id::id_type geometry_content_id,
                                     const id::id_type* material_ids, u32 material_count)
{
	if (!id::is_valid(id)) return;
	game_entity::entity entity{ entity_from_id(id) };
	if (!entity.is_valid()) return;
	mesh::component mc{ entity.mesh() };
	if (!mc.is_valid()) return;
	mesh::set_geometry(mc, geometry_content_id, material_ids, material_count);
}

// ============================================================================
// Simplified entity creation for Editor (Phase 5 Track A)
// ============================================================================

EDITOR_INTERFACE u32 CreateEntity(f32 pos_x, f32 pos_y, f32 pos_z)
{
	transform::init_info transform_info{};
	transform_info.position[0] = pos_x;
	transform_info.position[1] = pos_y;
	transform_info.position[2] = pos_z;
	// Identity quaternion: w=1, x=y=z=0
	transform_info.rotation[0] = 0.f;
	transform_info.rotation[1] = 0.f;
	transform_info.rotation[2] = 0.f;
	transform_info.rotation[3] = 1.f;
	transform_info.scale[0] = 1.f;
	transform_info.scale[1] = 1.f;
	transform_info.scale[2] = 1.f;

	script::init_info script_info{}; // no script

	game_entity::entity_info entity_info{};
	entity_info.transform = &transform_info;
	entity_info.script = &script_info;

	game_entity::entity ntt{ game_entity::create(entity_info) };
	if (!ntt.is_valid()) {
		return id::invalid_id;
	}
	return ntt.get_id();
}

EDITOR_INTERFACE void DestroyEntity(u32 entity_id)
{
	if (entity_id == id::invalid_id) return;
	game_entity::remove(game_entity::entity_id{ entity_id });
}
