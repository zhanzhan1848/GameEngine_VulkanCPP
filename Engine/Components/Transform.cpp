#include "Transform.h"
#include "Entity.h"

namespace primal::transform {

	namespace {

		utl::vector<math::m4x4>		to_world;
		utl::vector<math::m4x4>		inv_world;
		utl::vector<math::v4>		rotations;
		utl::vector<math::v3>		positions;
		utl::vector<math::v3>		orientations;
		utl::vector<math::v3>		scales;
		utl::vector<u8>				has_transform;
		utl::vector<u8>				changes_from_previous_frame;
		u8							read_write_flag;

		void calculate_transform_matrices(id::id_type index)
		{
			assert(rotations.size() >= index);
			assert(positions.size() >= index);
			assert(scales.size() >= index);
#if defined(_WIN32)
			using namespace DirectX;
			XMVECTOR r{ XMLoadFloat4(&rotations[index]) };
			XMVECTOR t{ XMLoadFloat3(&positions[index]) };
			XMVECTOR s{ XMLoadFloat3(&scales[index]) };

			XMMATRIX world{ XMMatrixAffineTransformation(s, XMQuaternionIdentity(), r, t) };
			XMStoreFloat4x4(&to_world[index], world);

			// NOTE: (F. Luna) Intro to DirectX 12, section 8.2.2
			world.r[3] = XMVectorSet(0.f, 0.f, 0.f, 1.f);
			XMMATRIX inverse_world{ XMMatrixInverse(nullptr, world) };
			XMStoreFloat4x4(&inv_world[index], inverse_world);
#elif defined(__APPLE__)
			using namespace simd;
			// 构建四元数 (w, x, y, z)
			simd::quatf r = simd_quaternion(rotations[index].x, rotations[index].y, rotations[index].z, rotations[index].w);
			math::v3 t = positions[index];
			math::v3 s = scales[index];

			// 从四元数创建旋转矩阵
			simd::float3x3 rotation_matrix = simd_matrix3x3(r);
			
			// 应用缩放
			simd::float3x3 scale_matrix = simd_matrix(
				simd_make_float3(s.x, 0.0f, 0.0f),
				simd_make_float3(0.0f, s.y, 0.0f),
				simd_make_float3(0.0f, 0.0f, s.z)
			);
			
			simd::float3x3 rs_matrix = simd_mul(rotation_matrix, scale_matrix);
			
			// 构建4x4变换矩阵
			simd::float4x4 world = simd_matrix(
				simd_make_float4(rs_matrix.columns[0].x, rs_matrix.columns[0].y, rs_matrix.columns[0].z, 0.0f),
				simd_make_float4(rs_matrix.columns[1].x, rs_matrix.columns[1].y, rs_matrix.columns[1].z, 0.0f),
				simd_make_float4(rs_matrix.columns[2].x, rs_matrix.columns[2].y, rs_matrix.columns[2].z, 0.0f),
				simd_make_float4(t.x, t.y, t.z, 1.0f)
			);

			to_world[index] = world;

			// 计算逆矩阵
			simd::float4x4 world_for_inverse = world;
			world_for_inverse.columns[3] = simd_make_float4(0.0f, 0.0f, 0.0f, 1.0f);
			simd::float4x4 inverse_world = simd_inverse(world_for_inverse);
			inv_world[index] = inverse_world;
#else
			// Portable fallback — also used by Emscripten/WASM (neither _WIN32
			// nor __APPLE__ matches). Builds world = T*R*S and computes the
			// affine inverse analytically as S^-1 * R^T * T^-1. Without this
			// branch the function body is empty on WASM — UB for a non-void
			// function, manifests as `unreachable` trap at runtime.
			{
				const math::v4 q = rotations[index];
				const math::v3 t = positions[index];
				const math::v3 s = scales[index];

				// Quaternion-to-rotation (column-major). Standard formula
				// for unit quaternion (x,y,z,w):
				//   R_col0 = (1-2(y²+z²),  2(xy+wz),    2(xz-wy))
				//   R_col1 = (2(xy-wz),    1-2(x²+z²),  2(yz+wx))
				//   R_col2 = (2(xz+wy),    2(yz-wx),    1-2(x²+y²))
				f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
				f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
				f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

				f32 r0x = 1.0f - 2.0f * (yy + zz);
				f32 r0y = 2.0f * (xy + wz);
				f32 r0z = 2.0f * (xz - wy);
				f32 r1x = 2.0f * (xy - wz);
				f32 r1y = 1.0f - 2.0f * (xx + zz);
				f32 r1z = 2.0f * (yz + wx);
				f32 r2x = 2.0f * (xz + wy);
				f32 r2y = 2.0f * (yz - wx);
				f32 r2z = 1.0f - 2.0f * (xx + yy);

				// World = T * R * S — scale each rotation column.
				math::m4x4 world;
				world.columns[0] = math::v4{ r0x * s.x, r0y * s.x, r0z * s.x, 0.0f };
				world.columns[1] = math::v4{ r1x * s.y, r1y * s.y, r1z * s.y, 0.0f };
				world.columns[2] = math::v4{ r2x * s.z, r2y * s.z, r2z * s.z, 0.0f };
				world.columns[3] = math::v4{ t.x, t.y, t.z, 1.0f };
				to_world[index] = world;

				// Inverse = S^-1 * R^T * T^-1.
				// S^-1*R^T rows: R_col_i / s_i. Translation: -(R_col_i · t) / s_i.
				f32 isx = (s.x != 0.0f) ? 1.0f / s.x : 0.0f;
				f32 isy = (s.y != 0.0f) ? 1.0f / s.y : 0.0f;
				f32 isz = (s.z != 0.0f) ? 1.0f / s.z : 0.0f;
				f32 inv_tx = -(r0x * t.x + r0y * t.y + r0z * t.z) * isx;
				f32 inv_ty = -(r1x * t.x + r1y * t.y + r1z * t.z) * isy;
				f32 inv_tz = -(r2x * t.x + r2y * t.y + r2z * t.z) * isz;

				math::m4x4 inverse_world;
				inverse_world.columns[0] = math::v4{ r0x * isx, r1x * isy, r2x * isz, 0.0f };
				inverse_world.columns[1] = math::v4{ r0y * isx, r1y * isy, r2y * isz, 0.0f };
				inverse_world.columns[2] = math::v4{ r0z * isx, r1z * isy, r2z * isz, 0.0f };
				inverse_world.columns[3] = math::v4{ inv_tx, inv_ty, inv_tz, 1.0f };
				inv_world[index] = inverse_world;
			}
#endif

			has_transform[index] = 1;
		}

		math::v3 calculate_orientation(math::v4 rotations)
		{
#if defined(_WIN32)
			using namespace DirectX;
			XMVECTOR rotation_quat{ XMLoadFloat4(&rotations) };
			XMVECTOR front{ XMVectorSet(0.f, 0.f, 1.f, 0.f) };
			math::v3 orientation;
			XMStoreFloat3(&orientation, XMVector3Rotate(front, rotation_quat));
			return orientation;
#elif defined(__APPLE__) && !defined(__EMSCRIPTEN__)
			using namespace simd;
			// 构建四元数 (w, x, y, z)
			simd::quatf r = simd_quaternion(rotations.x, rotations.y, rotations.z, rotations.w);
			// 前向向量
			simd::float3 front = simd_make_float3(0.0f, 0.0f, 1.0f);
			// 使用四元数旋转向量
			simd::float3 rotated_front = simd_act(r, front);
			// 转换为math::v3类型
			math::v3 orientation = simd_make_float3(rotated_front.x, rotated_front.y, rotated_front.z);
			return orientation;
#else
			// Portable fallback — also used by Emscripten/WASM build. Rotating
			// front=(0,0,1) by quaternion (x,y,z,w). Without this branch the
			// function body is empty on WASM — UB for non-void, traps as
			// `unreachable` at runtime.
			// Formula: v' = v + 2*w*cross(q.xyz, v) + 2*cross(q.xyz, cross(q.xyz, v))
			// cross(q.xyz, (0,0,1)) = (q.y, -q.x, 0); second cross simplifies:
			// cross(q.xyz, (q.y, -q.x, 0)) = (-q.x*q.z, -q.y*q.z, q.x² + q.y²)
			f32 qx = rotations.x, qy = rotations.y, qz = rotations.z, qw = rotations.w;
			f32 cx = qy;
			f32 cy = -qx;
			f32 cz = 0.0f;
			f32 tx = qy * cz - qz * cy;  // = qz*qx
			f32 ty = qz * cx - qx * cz;  // = qz*qy
			f32 tz = qx * cy - qy * cx;  // = -(qx² + qy²)
			return math::v3{
				2.0f * qw * cx + 2.0f * tx,
				2.0f * qw * cy + 2.0f * ty,
				1.0f + 2.0f * qw * cz + 2.0f * tz
			};
#endif
		}

		void set_rotation(transform_id id, const math::v4& rotation_quaternion)
		{
			const u32 index{ id::index(id) };
			rotations[index] = rotation_quaternion;
			orientations[index] = calculate_orientation(rotation_quaternion);
			has_transform[index] = 0;
			changes_from_previous_frame[index] |= component_flags::rotation;
		}

		void set_orientation(transform_id id, const math::v3&)
		{

		}

		void set_position(transform_id id, const math::v3& position)
		{
			const u32 index{ id::index(id) };
			positions[index] = position;
			has_transform[index] = 0;
			changes_from_previous_frame[index] |= component_flags::position;
		}

		void set_scale(transform_id id, const math::v3& scale)
		{
			const u32 index{ id::index(id) };
			scales[index] = scale;
			has_transform[index] = 0;
			changes_from_previous_frame[index] |= component_flags::scale;
		}

	} // anonymous namespace


	component create(init_info info, game_entity::entity entity)
	{
		assert(entity.is_valid());
		const id::id_type entity_index{ id::index(entity.get_id()) };

		if (positions.size() > entity_index)
		{
			math::v4 rotation{ info.rotation[0], info.rotation[1], info.rotation[2], info.rotation[3] };
			rotations[entity_index] = rotation;
			orientations[entity_index] = calculate_orientation(rotation);
			positions[entity_index] = math::v3{ info.position[0], info.position[1], info.position[2] };
			scales[entity_index] = math::v3{ info.scale[0], info.scale[1], info.scale[2] };
			has_transform[entity_index] = 0;
			changes_from_previous_frame[entity_index] = (u8)component_flags::all;
		}
		else
		{
			assert(positions.size() == entity_index);
			positions.emplace_back(math::v3{ info.position[0], info.position[1], info.position[2] });
			orientations.emplace_back(calculate_orientation(math::v4{ info.rotation[0], info.rotation[1], info.rotation[2], info.rotation[3] }));
			rotations.emplace_back(math::v4{ info.rotation[0], info.rotation[1], info.rotation[2], info.rotation[3] });
			scales.emplace_back(math::v3{ info.scale[0], info.scale[1], info.scale[2] });
			has_transform.emplace_back((u8)0);
			to_world.emplace_back();
			inv_world.emplace_back();
			changes_from_previous_frame.emplace_back((u8)component_flags::all);
		}

		// NOTE: each entity has a transform component. Therefore, id's for transform components
		//		 are exactly the same as entity ids.
		return component{ transform_id{ entity.get_id() } };
	}

	void remove([[maybe_unused]] component c)
	{
		assert(c.is_valid());
	}

	void get_transform_matrices(const game_entity::entity_id id, math::m4x4& world, math::m4x4& inverse_world)
	{
		assert(game_entity::entity{ id }.is_valid());

		const id::id_type entity_index{ id::index(id) };
		if (!has_transform[entity_index])
		{
			calculate_transform_matrices(entity_index);
		}

		world = to_world[entity_index];
		inverse_world = inv_world[entity_index];
	}

	void get_updated_components_flags(const game_entity::entity_id *const ids, u32 count, u8 *const flags)
	{
		assert(ids && count && flags);
		read_write_flag = 1;

		for (u32 i{ 0 }; i < count; ++i)
		{
			assert(game_entity::entity{ ids[i] }.is_valid());
			flags[i] = changes_from_previous_frame[id::index(ids[i])];
		}
	}

	void update(const component_cache *const cache, u32 count)
	{
		assert(cache && count);

		//  NOTE: clearing "changes_from_previous_frame" happens once every frame when there will be no reads and the caches are
		//		 about to be applied by calling this function( i.e. the rest of the current frame will only have writes.)
		if (read_write_flag)
		{
			memset(changes_from_previous_frame.data(), 0, changes_from_previous_frame.size());
			read_write_flag = 0;
		}

		for (u32 i{ 0 }; i < count; ++i)
		{
			const component_cache& c{ cache[i] };
			assert(component{ c.id }.is_valid());

			if (c.flags & component_flags::rotation)
			{
				set_rotation(c.id, c.rotation);
			}

			if (c.flags & component_flags::orientation)
			{
				set_orientation(c.id, c.orientation);
			}

			if (c.flags & component_flags::position)
			{
				set_position(c.id, c.position);
			}

			if (c.flags & component_flags::scale)
			{
				set_scale(c.id, c.scale);
			}
		}
	}

	math::v4 component::rotation() const
	{
		assert(is_valid());
		return rotations[id::index(_id)];
	}

	math::v3 component::orientation() const
	{
		assert(is_valid());
		return orientations[id::index(_id)];
	}

	math::v3 component::position() const
	{
		assert(is_valid());
		return positions[id::index(_id)];
	}

	math::v3 component::scale() const
	{
		assert(is_valid());
		return scales[id::index(_id)];
	}
}