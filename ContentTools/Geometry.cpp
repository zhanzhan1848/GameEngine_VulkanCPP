#include "Geometry.h"
#include "meshoptimizer.h"
#include "../Engine/Utilities/IOStream.h"
#include "../Engine/Graphics/RHI/Core/RHIMath.h"
#include <cmath>
#include <iostream>
#include <algorithm> // for std::clamp in C++17
#include <cfloat>

namespace primal::tools
{
	namespace 
	{
		using namespace primal::math;
		using namespace primal::graphics::rhi::math;

		inline v3 Min(const v3& a, const v3& b)
		{
			return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
		}

		inline v3 Max(const v3& a, const v3& b)
		{
			return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
		}

		void recalculate_normals(mesh& m)
		{
			const u32 num_indices{ (u32)m.raw_indices.size() };
			m.normals.resize(num_indices);

			for (u32 i{ 0 }; i < num_indices; ++i)
			{
				const u32 i0{ m.raw_indices[i] };
				const u32 i1{ m.raw_indices[++i] };
				const u32 i2{ m.raw_indices[++i] };

				v3 v0 = m.positions[i0];
				v3 v1 = m.positions[i1];
				v3 v2 = m.positions[i2];

				v3 e0 = v1 - v0;
				v3 e1 = v2 - v0;

				v3 n = Cross(e0, e1);
				if (LengthSquared(n) > epsilon)
				{
					n = Normalize(n);
				}
				else
				{
					n = { 0.f, 0.f, 0.f };
				}
				m.normals[i] = n;

				m.normals[i - 1] = m.normals[i];
				m.normals[i - 2] = m.normals[i];
			}
		}

		void process_normals(mesh& m, f32 smoothing_angle)
		{
#if defined(_MSC_VER)
			const f32 cos_alpha{ XMScalarCos(pi - smoothing_angle * pi / 180.f) };
			const bool is_hard_edge{ XMScalarNearEqual(smoothing_angle, 180.f, epsilon) };
			const bool is_soft_edge{ XMScalarNearEqual(smoothing_angle, 0.f, epsilon) };
			const u32 num_indices{ (u32)m.raw_indices.size() };
			const u32 num_vertices{ (u32)m.positions.size() };
			assert(num_indices && num_vertices);

			m.indices.resize(num_indices);

			utl::vector<utl::vector<u32>> idx_ref(num_vertices);
			for (u32 i{ 0 }; i < num_indices; ++i) idx_ref[m.raw_indices[i]].emplace_back(i);
			for (u32 i{ 0 }; i < num_vertices; ++i)
			{
				auto& refs{ idx_ref[i] };
				u32 num_refs{ (u32)refs.size() };
				for (u32 j{ 0 }; j < num_refs; ++j)
				{
					m.indices[refs[j]] = (u32)m.vertices.size();
					vertex& v{ m.vertices.emplace_back() };
					v.position = m.positions[m.raw_indices[refs[j]]];

					XMVECTOR n1{ XMLoadFloat3(&m.normals[refs[j]]) };
					if (!is_hard_edge)
					{
						for (u32 k{ j + 1 }; k < num_refs; ++k)
						{
							// this value represents the cosine of the angle between nromals
							f32 cos_theta{ 0.f };
							XMVECTOR n2{ XMLoadFloat3(&m.normals[refs[k]]) };
							if (!is_soft_edge)
							{
								// Note: we're accounting for the length of n1 in this calculation because
								//		it can possibly change in this loop iteration. We assume unit length
								//		for n2.
								//		cos(angle) = dot(n1, n2) / (||n1|| + ||n2||)
								XMStoreFloat(&cos_theta, XMVector3Dot(n1, n2) * XMVector3ReciprocalLength(n1));
							}

							if (is_soft_edge || cos_theta >= cos_alpha)
							{
								n1 += n2;
								m.indices[refs[k]] = m.indices[refs[j]];
								refs.erase(refs.begin() + k);
								--num_refs;
								--k;
							}
						}
					}
					XMStoreFloat3(&v.normal, XMVector3Normalize(n1));
				}
			}
#elif defined(__clang__)
			const f32 cos_alpha{ std::cos(pi - smoothing_angle * pi / 180.f) };
			const bool is_hard_edge{ std::abs(smoothing_angle - 180.f) < epsilon };
			const bool is_soft_edge{ std::abs(smoothing_angle) < epsilon };

			const u32 num_indices{ (u32)m.raw_indices.size() };
			const u32 num_vertices{ (u32)m.positions.size() };
			assert(num_indices && num_vertices);

			m.indices.resize(num_indices);

			utl::vector<utl::vector<u32>> idx_ref(num_vertices);
			for(u32 i{ 0 }; i < num_indices; ++i) idx_ref[m.raw_indices[i]].emplace_back(i);
			
			for(u32 i{ 0 }; i < num_vertices; ++i)
			{
				auto& refs{ idx_ref[i] };
				u32 num_refs{ (u32)refs.size() };
				for(u32 j{ 0 }; j < num_refs; ++j)
				{
					m.indices[refs[j]] = (u32)m.vertices.size();
					vertex& v{ m.vertices.emplace_back() };
					v.position = m.positions[m.raw_indices[refs[j]]];

					math::v3 n1{ m.normals[refs[j]].x, m.normals[refs[j]].y, m.normals[refs[j]].z };
					if(!is_hard_edge)
					{
						for(u32 k{ j + 1 }; k < num_refs; ++k)
						{
							f32 cos_theta{ 0.f };
							math::v3 n2{ m.normals[refs[k]].x, m.normals[refs[k]].y, m.normals[refs[k]].z };
							if(!is_soft_edge)
							{

								cos_theta = simd_dot(n1, n2) / (simd_length(n1) + simd_length(n2));
							}
							
							if(is_soft_edge || cos_theta >= cos_alpha)
							{
								n1 += n2;
								m.indices[refs[k]] = m.indices[refs[j]];
								refs.erase(refs.begin() + k);
								--num_refs;
								--k;
							}
						}
					}
					if (simd_length_squared(n1) > epsilon)
					{
						n1 = simd_normalize(n1);
					}
					else
					{
						n1 = { 0.f, 1.f, 0.f };
					}
					v.normal = { n1.x, n1.y, n1.z };
				}
			}
#endif
		}

		void process_uvs(mesh& m)
		{
			utl::vector<vertex> old_vertices;
			old_vertices.swap(m.vertices);
			utl::vector<u32> old_indices(m.indices.size());
			old_indices.swap(m.indices);

			const u32 num_vertices{ (u32)old_vertices.size() };
			const u32 num_indices{ (u32)old_indices.size() };
			assert(num_vertices && num_indices);

			utl::vector<utl::vector<u32>> idx_ref(num_vertices);
			for (u32 i{ 0 }; i < num_indices; ++i) idx_ref[old_indices[i]].emplace_back(i);

			for (u32 i{ 0 }; i < num_vertices; ++i)
			{
				auto& refs{ idx_ref[i] };
				u32 num_refs{ (u32)refs.size() };
				for (u32 j{ 0 }; j < num_refs; ++j)
				{
					m.indices[refs[j]] = (u32)m.vertices.size();
					vertex& v{ old_vertices[old_indices[refs[j]]] };
					v.uv = m.uv_sets[0][refs[j]];
					m.vertices.emplace_back(v);

					for (u32 k{ j + 1 }; k < num_refs; ++k)
					{
						v2& uv1{ m.uv_sets[0][refs[k]] };
#if defined(_MSC_VER)
						if (XMScalarNearEqual(v.uv.x, uv1.x, epsilon) &&
							XMScalarNearEqual(v.uv.y, uv1.y, epsilon))
#elif defined(__clang__)
						if (std::abs(v.uv.x - uv1.x) < epsilon &&
							std::abs(v.uv.y - uv1.y) < epsilon)
#endif
						{
							m.indices[refs[k]] = m.indices[refs[j]];
							refs.erase(refs.begin() + k);
							--num_refs;
							--k;
						}
					}
				}
			}
		}

		u64 get_vertex_element_size(elements::elements_type::type elements_type)
		{
			using namespace elements;
			switch (elements_type)
			{
			case elements_type::static_normal:						return sizeof(static_normal);
			case elements_type::static_normal_texture:				return sizeof(static_normal_texture);
			case elements_type::static_color:						return sizeof(static_color);
			case elements_type::skeletal:							return sizeof(skeletal);
			case elements_type::skeletal_color:						return sizeof(skeletal_color);
			case elements_type::skeletal_normal:					return sizeof(skeletal_normal);
			case elements_type::skeletal_normal_color:				return sizeof(skeletal_normal_color);
			case elements_type::skeletal_normal_texture:			return sizeof(skeletal_normal_texture);
			case elements_type::skeletal_normal_texture_color:		return sizeof(skeletal_normal_texture_color);
			}

			return 0;
		}

		math::v3 closest_point_triangle(const math::v3& p, const math::v3& a, const math::v3& b, const math::v3& c)
		{
			math::v3 ab = b - a;
			math::v3 ac = c - a;
			math::v3 ap = p - a;
			f32 d1 = dot(ab, ap);
			f32 d2 = dot(ac, ap);
			if (d1 <= 0.0f && d2 <= 0.0f) return a;

			math::v3 bp = p - b;
			f32 d3 = dot(ab, bp);
			f32 d4 = dot(ac, bp);
			if (d3 >= 0.0f && d4 <= d3) return b;

			f32 vc = d1 * d4 - d3 * d2;
			if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
			{
				f32 v = d1 / (d1 - d3);
				return a + ab * v;
			}

			math::v3 cp = p - c;
			f32 d5 = dot(ab, cp);
			f32 d6 = dot(ac, cp);
			if (d6 >= 0.0f && d5 <= d6) return c;

			f32 vb = d5 * d2 - d1 * d6;
			if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
			{
				f32 w = d2 / (d2 - d6);
				return a + ac * w;
			}

			f32 va = d3 * d6 - d5 * d4;
			if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
			{
				f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
				return b + (c - b) * w;
			}

			f32 denom = 1.0f / (va + vb + vc);
			f32 v = vb * denom;
			f32 w = vc * denom;
			return a + ab * v + ac * w;
		}

		void process_meshlets(mesh& m)
		{
			if (m.indices.empty()) return;

			const size_t max_meshlets = meshopt_buildMeshletsBound(m.indices.size(), 64, 124);
			utl::vector<meshopt_Meshlet> local_meshlets(max_meshlets);
			utl::vector<u32> local_meshlet_vertices(max_meshlets * 64);
			utl::vector<u8> local_meshlet_triangles(max_meshlets * 124 * 3);

			size_t meshlet_count = meshopt_buildMeshlets(
				local_meshlets.data(), local_meshlet_vertices.data(), local_meshlet_triangles.data(),
				m.indices.data(), m.indices.size(),
				(float*)&m.vertices[0].position, m.vertices.size(), sizeof(vertex),
				64, 124, 0.5f
			);

			const meshopt_Meshlet* meshlets = local_meshlets.data();
			m.meshlets.resize(meshlet_count);
			m.meshlet_vertices.clear();
			m.meshlet_triangles.clear();

			for (size_t i = 0; i < meshlet_count; ++i)
			{
				const meshopt_Meshlet& ml = meshlets[i];
				mesh::meshlet& new_ml = m.meshlets[i];

				new_ml.vertex_offset = (u32)m.meshlet_vertices.size();
				new_ml.triangle_offset = (u32)m.meshlet_triangles.size();
				new_ml.vertex_count = ml.vertex_count;
				new_ml.triangle_count = ml.triangle_count;

				// Copy vertices
				for (u32 j = 0; j < ml.vertex_count; ++j)
				{
					m.meshlet_vertices.push_back(local_meshlet_vertices[ml.vertex_offset + j]);
				}

				// Copy triangles (packed)
				for (u32 j = 0; j < ml.triangle_count * 3; ++j)
				{
					m.meshlet_triangles.push_back(local_meshlet_triangles[ml.triangle_offset + j]);
				}

				// Compute bounds
				meshopt_Bounds bounds = meshopt_computeMeshletBounds(
					&local_meshlet_vertices[ml.vertex_offset],
					&local_meshlet_triangles[ml.triangle_offset],
					ml.triangle_count,
					(float*)&m.vertices[0].position, m.vertices.size(), sizeof(vertex)
				);

				new_ml.center[0] = bounds.center[0];
				new_ml.center[1] = bounds.center[1];
				new_ml.center[2] = bounds.center[2];
				new_ml.radius = bounds.radius;
				new_ml.cone_apex[0] = bounds.cone_apex[0];
				new_ml.cone_apex[1] = bounds.cone_apex[1];
				new_ml.cone_apex[2] = bounds.cone_apex[2];
				new_ml.cone_axis[0] = bounds.cone_axis[0];
				new_ml.cone_axis[1] = bounds.cone_axis[1];
				new_ml.cone_axis[2] = bounds.cone_axis[2];
				new_ml.cone_cutoff = bounds.cone_cutoff;
			}
		}

		void generate_sdf(mesh& m)
		{
			// Simple grid based SDF
			constexpr u32 res = 32;
			m.sdf.resolution[0] = res;
			m.sdf.resolution[1] = res;
			m.sdf.resolution[2] = res;

			math::v3 min_b{ FLT_MAX, FLT_MAX, FLT_MAX };
			math::v3 max_b{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

			for (const auto& v : m.vertices)
			{
				min_b = Min(min_b, v.position);
				max_b = Max(max_b, v.position);
			}

			math::v3 size = max_b - min_b;
			f32 max_dim = std::max({ size.x, size.y, size.z });
			if (max_dim < 0.0001f) max_dim = 1.0f;
			f32 padding = max_dim * 0.2f;
			min_b -= padding;
			max_b += padding;
			size = max_b - min_b;

			m.sdf.bounds_min[0] = min_b.x;
			m.sdf.bounds_min[1] = min_b.y;
			m.sdf.bounds_min[2] = min_b.z;
			m.sdf.bounds_max[0] = max_b.x;
			m.sdf.bounds_max[1] = max_b.y;
			m.sdf.bounds_max[2] = max_b.z;

			m.sdf.data.resize(res * res * res);
			m.sdf.voxels.resize(res * res * res);
			m.sdf.vector_field.resize(res * res * res * 4);

			math::v3 step = size / (f32)res;

			// Precompute triangles
			u32 num_indices = (u32)m.indices.size();

			for (u32 z = 0; z < res; ++z)
			{
				for (u32 y = 0; y < res; ++y)
				{
					for (u32 x = 0; x < res; ++x)
					{
						math::v3 p = min_b + step * (math::v3{ (f32)x, (f32)y, (f32)z } + 0.5f);
						f32 min_dist_sq = FLT_MAX;
						math::v3 closest_p = p;

						for (u32 i = 0; i < num_indices; i += 3)
						{
							math::v3 v0 = m.vertices[m.indices[i]].position;
							math::v3 v1 = m.vertices[m.indices[i + 1]].position;
							math::v3 v2 = m.vertices[m.indices[i + 2]].position;

							math::v3 cp = closest_point_triangle(p, v0, v1, v2);
							f32 d2 = LengthSquared(p - cp);
							if (d2 < min_dist_sq) 
							{
								min_dist_sq = d2;
								closest_p = cp;
							}
						}

						f32 dist = std::sqrt(min_dist_sq);
						u32 idx = z * res * res + y * res + x;
						m.sdf.data[idx] = math::pack_float<16>(dist, 0.0f, max_dim);
						
						// Voxel: occupied if close enough
						f32 voxel_diag = Length(step);
						m.sdf.voxels[idx] = (dist < voxel_diag * 0.5f) ? 255 : 0;
						
						// Vector Field: vector to closest point
						math::v3 to_closest = closest_p - p;
						u32 vec_idx = idx * 4;
						m.sdf.vector_field[vec_idx + 0] = math::pack_float<16>(to_closest.x, -max_dim, max_dim);
						m.sdf.vector_field[vec_idx + 1] = math::pack_float<16>(to_closest.y, -max_dim, max_dim);
						m.sdf.vector_field[vec_idx + 2] = math::pack_float<16>(to_closest.z, -max_dim, max_dim);
						m.sdf.vector_field[vec_idx + 3] = 0;
					}
				}
			}
		}

		void pack_vertices(mesh& m)
		{
			const u32 num_vertices{ (u32)m.vertices.size() };
			assert(num_vertices);

			m.position_buffer.resize(12 * num_vertices);
			f32* position_buffer{ (f32*)m.position_buffer.data() };

			for (u32 i{ 0 }; i < num_vertices; ++i)
			{
				position_buffer[i * 3 + 0] = m.vertices[i].position.x;
				position_buffer[i * 3 + 1] = m.vertices[i].position.y;
				position_buffer[i * 3 + 2] = m.vertices[i].position.z;
			}

			struct u16v2
			{
				u16 x, y;
			};

			struct u8v3
			{
				u8 x, y, z;
			};
			utl::vector<u8> t_signs(num_vertices);
			utl::vector<u16v2> normals(num_vertices);
			utl::vector<u16v2> tangents(num_vertices);
			utl::vector<u8v3> joint_weights(num_vertices);

			if (m.elements_type & elements::elements_type::static_normal)
			{
				// normal only
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
#if defined(_MSC_VER)
					t_signs[i] = (u8)((v.normal.z > 0.f) << 1);
					normals[i] = { (u16)pack_float<16>(clamp(v.normal.x, -1.f, 1.f), -1.f, 1.f), (u16)pack_float<16>(clamp(v.normal.y, -1.f, 1.f), -1.f, 1.f) };
#elif defined(__clang__)
					if (!std::isfinite(v.normal.x) || !std::isfinite(v.normal.y) || !std::isfinite(v.normal.z))
					{
						v.normal = { 0.f, 1.f, 0.f };
					}
					t_signs[i] = (u8)((v.normal.z > 0.f) << 1);
					normals[i] = { (u16)pack_float<16>(math::clamp(v.normal.x, -1.f, 1.f), -1.f, 1.f), (u16)pack_float<16>(math::clamp(v.normal.y, -1.f, 1.f), -1.f, 1.f) };
#endif
				}

				if (m.elements_type & elements::elements_type::static_normal_texture)
				{
					// full T-space
					for (u32 i{ 0 }; i < num_vertices; ++i)
					{
						vertex& v{ m.vertices[i] };
#if defined(_MSC_VER)
						t_signs[i] = (u8)((v.tangent.w > 0.f) && (v.tangent.z > 0.f));
						tangents[i] = { (u16)pack_float<16>(clamp(v.tangent.x, -1.f, 1.f), -1.f, 1.f), (u16)pack_float<16>(clamp(v.tangent.y, -1.f, 1.f), -1.f, 1.f) };
#elif defined(__clang__)
						if (!std::isfinite(v.tangent.x) || !std::isfinite(v.tangent.y) || !std::isfinite(v.tangent.z))
						{
							v.tangent = { 0.f, 1.f, 0.f, 1.f };
						}
						t_signs[i] = (u8)((v.tangent.w > 0.f) && (v.tangent.z > 0.f));
						tangents[i] = { (u16)pack_float<16>(math::clamp(v.tangent.x, -1.f, 1.f), -1.f, 1.f), (u16)pack_float<16>(math::clamp(v.tangent.y, -1.f, 1.f), -1.f, 1.f) };
#endif	
					}
				}
			}

			if (m.elements_type & elements::elements_type::skeletal)
			{
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
					// pack joint weighrts (from [0.0, 1.0] to [0..255])
#if defined(_MSC_VER)
					joint_weights[i] = {
						(u8)pack_unit_float<8>(v.joint_weights.x),
						(u8)pack_unit_float<8>(v.joint_weights.y),
						(u8)pack_unit_float<8>(v.joint_weights.z)
					};
#elif defined(__clang__)
					if (!std::isfinite(v.joint_weights.x) || !std::isfinite(v.joint_weights.y) || !std::isfinite(v.joint_weights.z))
					{
						v.joint_weights = { 0.f, 0.f, 0.f };
					}
					joint_weights[i] = {
						(u8)pack_unit_float<8>(math::clamp(v.joint_weights.x, 0.f, 1.f)),
						(u8)pack_unit_float<8>(math::clamp(v.joint_weights.y, 0.f, 1.f)),
						(u8)pack_unit_float<8>(math::clamp(v.joint_weights.z, 0.f, 1.f))
					};
#endif

					// NOTE: w3 will be calculated in shader since joint weights sun to one(1).
				}
			}

			m.element_buffer.resize(get_vertex_element_size(m.elements_type) * num_vertices);
			using namespace elements;

			switch (m.elements_type)
			{
			case elements_type::static_color: 
			{
				static_color *const element_buffer{ (static_color *const)m.element_buffer.data() };
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
					element_buffer[i] = { {v.red, v.green, v.blue}, {} };
				}
			} break;
			case elements_type::static_normal:						
			{
				static_normal *const element_buffer{ (static_normal *const)m.element_buffer.data() };
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
					element_buffer[i] = { {v.red, v.green, v.blue}, t_signs[i], {normals[i].x, normals[i].y} };
				}
			} break;
			case elements_type::static_normal_texture:				
			{
				static_normal_texture *const element_buffer{ (static_normal_texture *const)m.element_buffer.data() };
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
					element_buffer[i] = { {v.red, v.green, v.blue}, t_signs[i],
						{normals[i].x, normals[i].y}, {tangents[i].x, tangents[i].y}, v.uv };
				}
			} break;
			case elements_type::skeletal:							
			{
				skeletal *const element_buffer{ (skeletal *const)m.element_buffer.data() };
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
#if defined(_MSC_VER)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#elif defined(__clang__)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#endif
					element_buffer[i] = { {joint_weights[i].x, joint_weights[i].y, joint_weights[i].z}, {}, {indices[0], indices[1], indices[2], indices[3]} };
				}
			} break;
			case elements_type::skeletal_color:						
			{
				skeletal_color *const element_buffer{ (skeletal_color *const)m.element_buffer.data() };
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
#if defined(_MSC_VER)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#elif defined(__clang__)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#endif
					element_buffer[i] = { {joint_weights[i].x, joint_weights[i].y, joint_weights[i].z}, {},
						{indices[0], indices[1], indices[2], indices[3]}, {v.red, v.green, v.blue}, {} };
				}
			} break;
			case elements_type::skeletal_normal:					
			{
				skeletal_normal *const element_buffer{ (skeletal_normal *const)m.element_buffer.data() };
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
#if defined(_MSC_VER)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#elif defined(__clang__)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#endif
					element_buffer[i] = { {joint_weights[i].x, joint_weights[i].y, joint_weights[i].z}, t_signs[i],
						{indices[0], indices[1], indices[2], indices[3]}, {normals[i].x, normals[i].y} };
				}
			} break;
			case elements_type::skeletal_normal_color:				
			{
				skeletal_normal_color *const element_buffer{ (skeletal_normal_color *const)m.element_buffer.data() };
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
#if defined(_MSC_VER)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#elif defined(__clang__)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#endif
					element_buffer[i] = { {joint_weights[i].x, joint_weights[i].y, joint_weights[i].z}, t_signs[i],
						{indices[0], indices[1], indices[2], indices[3]}, {normals[i].x, normals[i].y}, {v.red, v.green, v.blue}, {} };
				}
			} break;
			case elements_type::skeletal_normal_texture:			
			{
				skeletal_normal_texture *const element_buffer{ (skeletal_normal_texture *const)m.element_buffer.data() };
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
#if defined(_MSC_VER)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#elif defined(__clang__)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#endif
					element_buffer[i] = { {joint_weights[i].x, joint_weights[i].y, joint_weights[i].z}, t_signs[i],
						{indices[0], indices[1], indices[2], indices[3]}, {normals[i].x, normals[i].y}, {tangents[i].x, tangents[i].y}, v.uv };
				}
			} break;
			case elements_type::skeletal_normal_texture_color:		
			{
				skeletal_normal_texture_color *const element_buffer{ (skeletal_normal_texture_color *const)m.element_buffer.data() };
				for (u32 i{ 0 }; i < num_vertices; ++i)
				{
					vertex& v{ m.vertices[i] };
#if defined(_MSC_VER)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#elif defined(__clang__)
					const u16 indices[4]{ (u16)v.joint_indices.x, (u16)v.joint_indices.y, (u16)v.joint_indices.z, (u16)v.joint_indices.w };
#endif
					element_buffer[i] = { {joint_weights[i].x, joint_weights[i].y, joint_weights[i].z}, t_signs[i],
						{indices[0], indices[1], indices[2], indices[3]}, {normals[i].x, normals[i].y}, {tangents[i].x, tangents[i].y}, v.uv, {v.red, v.green, v.blue}, {} };
				}
			} break;
			}
		}

		elements::elements_type::type determine_elements_type(const mesh& m)
		{
			using namespace elements;
			elements_type::type type{};
			if (m.normals.size())
			{
				if (m.uv_sets.size() && m.uv_sets[0].size())
				{
					type = elements_type::static_normal_texture;
				}
				else
				{
					type = elements_type::static_normal;
				}
			}
			else if (m.colors.size())
			{
				type = elements_type::static_color;
			}

			// TODO: we lack data for skeletal meshes. Expand for skeletal meshes later.
			return type;
		}

		void process_vertices(mesh& m, const geometry_import_settings& settings)
		{
			assert((m.raw_indices.size() % 3) == 0);
			if (settings.calculate_normals || m.normals.empty())
			{
				recalculate_normals(m);
			}

			process_normals(m, settings.smoothing_angle);

			if (!m.uv_sets.empty())
			{
				process_uvs(m);
			}
			
			m.elements_type = determine_elements_type(m);

			process_meshlets(m);
			generate_sdf(m);

			pack_vertices(m);
		}

		u64 get_mesh_size(const mesh& m)
		{
			const u64 num_vertices{ m.vertices.size() };
			const u64 position_buffer_size{ m.position_buffer.size() };
			assert(position_buffer_size == 12 * num_vertices);
			const u64 element_buffer_size{ m.element_buffer.size() };
			assert(element_buffer_size == get_vertex_element_size(m.elements_type) * num_vertices);
			
			const u64 index_size{ (num_vertices < (1 << 16)) ? sizeof(u16) : sizeof(u32) };
			const u64 index_buffer_size{ index_size * m.indices.size() };
			constexpr u64 su32{ sizeof(u32) };
			const u64 size{
				su32 + m.name.size() +							// mesh name lenght and room for mesh name string
				su32 +											// LOD id
				su32 +											// material index
				su32 +											// vertex element size (vertex size excluding position element)
				su32 +											// element type enumeration
				su32 +											// number of vertices
				su32 +											// index size (16bits or 32 bits)
				su32 +											// number of indices
				sizeof(u32) +									// LOD threshold
				position_buffer_size +							// room for vertex position
				element_buffer_size +							// room for vertex element
				index_buffer_size +								// room for indices
				sizeof(u32) +									// magic_mshl
				sizeof(u32) +									// meshlet count
				sizeof(mesh::meshlet) * m.meshlets.size() +		// meshlets
				sizeof(u32) +									// meshlet vertex count
				sizeof(u32) * m.meshlet_vertices.size() +		// meshlet vertices
				sizeof(u32) +									// meshlet triangle count
				sizeof(u8) * m.meshlet_triangles.size() +		// meshlet triangles
				sizeof(u32) +									// magic_sdf
				sizeof(u32) * 3 +								// sdf resolution
				sizeof(f32) * 6 +								// sdf bounds
				sizeof(u32) +									// sdf data size
				sizeof(u16) * m.sdf.data.size() +				// sdf data
				sizeof(u32) +									// voxels size
				sizeof(u8) * m.sdf.voxels.size() +				// voxels
				sizeof(u32) +									// vector field size
				sizeof(u16) * m.sdf.vector_field.size()			// vector field
			};
			return size;
		}

		u64 get_scene_size(const scene& scene)
		{
			constexpr u64 su32{ sizeof(u32) };
			u64 size
			{
				su32 +											// name length
				scene.name.size() +								// room for scene name string
				su32											// number of materials
			};

			for (const auto& m : scene.materials)
			{
				size += su32 + m.name.size();
				size += su32 + m.diffuse_texture.size();
				size += su32 + m.normal_texture.size();
			}

			size += su32;										// number of LODs

			for (const auto& lod : scene.lod_groups)
			{
				u64 lod_size
				{
					su32 + lod.name.size() +					// LOD name length and room for LOD name string
					su32										// number of meshes in this LOD
				};

				for (const auto& m : lod.meshes)
				{
					lod_size += get_mesh_size(m);
				}

				size += lod_size;
			}

			return size;
		}

		void pack_mesh_data(const mesh& m, utl::blob_stream_writer& blob)
		{
			// mesh name
			blob.write((u32)m.name.size());
			blob.write(m.name.c_str(), m.name.size());
			// Lod id
			blob.write(m.lod_id);
			// material id
			blob.write(m.material_idx);
			// vertex elements size
			const u32 elements_size{ (u32)get_vertex_element_size(m.elements_type) };
			blob.write(elements_size);
			// elements type enumeration
			blob.write((u32)m.elements_type);
			// number of vertices
			const u32 num_vertices{ (u32)m.vertices.size() };
			blob.write(num_vertices);
			// index size (16 bits or 32 bits)
			const u32 index_size{ static_cast<u32>((num_vertices < (1 << 16)) ? sizeof(u16) : sizeof(u32)) };
			blob.write(index_size);
			// number of indices
			const u32 num_indices{ (u32)m.indices.size() };
			blob.write(num_indices);
			// LOD threshold
			blob.write(m.lod_threshold);
			// position buffer
			assert(m.position_buffer.size() == 12 * num_vertices);
			blob.write(m.position_buffer.data(), m.position_buffer.size());
			// element buffer
			assert(m.element_buffer.size() == elements_size * num_vertices);
			blob.write(m.element_buffer.data(), m.element_buffer.size());
			// index data
			const u32 index_buffer_size{ index_size * num_indices };
			const u8* data{ (const u8*)m.indices.data() };
			utl::vector<u16> indices;

			if (index_size == sizeof(u16))
			{
				indices.resize(num_indices);
				for (u32 i{ 0 }; i < num_indices; ++i) indices[i] = (u16)m.indices[i];
				data = (const u8*)indices.data();
			}
			blob.write(data, index_buffer_size);

			// Magic MSHL
			constexpr u32 magic_mshl{ 0x4C48534D }; // "MSHL"
			blob.write(magic_mshl);

			// Meshlets
			blob.write((u32)m.meshlets.size());
			if (!m.meshlets.empty())
				blob.write((const char*)m.meshlets.data(), m.meshlets.size() * sizeof(mesh::meshlet));

			// Meshlet vertices
			blob.write((u32)m.meshlet_vertices.size());
			if (!m.meshlet_vertices.empty())
				blob.write((const char*)m.meshlet_vertices.data(), m.meshlet_vertices.size() * sizeof(u32));

			// Meshlet triangles
			blob.write((u32)m.meshlet_triangles.size());
			if (!m.meshlet_triangles.empty())
				blob.write((const char*)m.meshlet_triangles.data(), m.meshlet_triangles.size() * sizeof(u8));

			// Magic SDF
			constexpr u32 magic_sdf{ 0x20464453 }; // "SDF "
			blob.write(magic_sdf);

			// SDF
			blob.write((const char*)m.sdf.resolution, sizeof(u32) * 3);
			blob.write((const char*)m.sdf.bounds_min, sizeof(f32) * 3);
			blob.write((const char*)m.sdf.bounds_max, sizeof(f32) * 3);
			blob.write((u32)m.sdf.data.size());
			if (!m.sdf.data.empty())
				blob.write((const char*)m.sdf.data.data(), m.sdf.data.size() * sizeof(u16));
			
			// Voxels
			blob.write((u32)m.sdf.voxels.size());
			if (!m.sdf.voxels.empty())
				blob.write((const char*)m.sdf.voxels.data(), m.sdf.voxels.size() * sizeof(u8));

			// Vector Field
			blob.write((u32)m.sdf.vector_field.size());
			if (!m.sdf.vector_field.empty())
				blob.write((const char*)m.sdf.vector_field.data(), m.sdf.vector_field.size() * sizeof(u16));
		}

		bool split_meshes_by_material(u32 material_idx, const mesh& m, mesh& submesh)
		{
			submesh.name = m.name;
			submesh.lod_threshold = m.lod_threshold;
			submesh.lod_id = m.lod_id;
			submesh.material_idx = material_idx;
			submesh.material_used.emplace_back(material_idx);
			submesh.uv_sets.resize(m.uv_sets.size());

			const u32 num_polys{ (u32)m.raw_indices.size() / 3 };
			utl::vector<u32> vertex_ref(m.positions.size(), u32_invalid_id);

			for (u32 i{ 0 }; i < num_polys; ++i)
			{
				const u32 mtl_idx{ m.material_indices[i] };
				if (mtl_idx != material_idx) continue;

				const u32 index{ i * 3 };
				for (u32 j = index; j < index + 3; ++j)
				{
					const u32 v_idx{ m.raw_indices[j] };
					if (vertex_ref[v_idx] != u32_invalid_id)
					{
						submesh.raw_indices.emplace_back(vertex_ref[v_idx]);
					}
					else
					{
						submesh.raw_indices.emplace_back((u32)submesh.positions.size());
						vertex_ref[v_idx] = submesh.raw_indices.back();
						submesh.positions.emplace_back(m.positions[v_idx]);
					}

					if (m.normals.size())
					{
						submesh.normals.emplace_back(m.normals[j]);
					}

					if (m.tangents.size())
					{
						submesh.tangents.emplace_back(m.tangents[j]);
					}

					for (u32 k{ 0 }; k < m.uv_sets.size(); ++k)
					{
						if (m.uv_sets[k].size())
						{
							submesh.uv_sets[k].emplace_back(m.uv_sets[k][j]);
						}
					}
				}
			}

			assert((submesh.raw_indices.size() % 3) == 0);
			return !submesh.raw_indices.empty();
		}

		void split_meshes_by_material(scene& scene, progression *const progression)
		{
			assert(progression);
			progression->callback(0, 0);

			for (auto& lod : scene.lod_groups)
			{
				utl::vector<mesh> new_meshes;

				for (const auto& m : lod.meshes)
				{
					// If more than one material is used in this mesh
					// then split it into submeshes.
					const u32 num_materials{ (u32)m.material_used.size() };
					if (num_materials > 1)
					{
						for (u32 i{ 0 }; i < num_materials; ++i)
						{
							mesh submesh{};
							if (split_meshes_by_material(m.material_used[i], m, submesh))
							{
								new_meshes.emplace_back(submesh);
							}
						}
					}
					else
					{
						mesh copy = m;
						if (num_materials == 1)
						{
							copy.material_idx = m.material_used[0];
						}
						new_meshes.emplace_back(copy);
					}
				}
				progression->callback(progression->value(), progression->max_value() + (u32)new_meshes.size());
				new_meshes.swap(lod.meshes);
			}
		}

		template<typename T> void append_to_vector_pod(utl::vector<T>& dst, const utl::vector<T>& src)
		{
			if (src.empty()) return;
			const u32 num_elements{ (u32)dst.size() };
			dst.resize(dst.size() + src.size());
			memcpy(&dst[num_elements], src.data(), src.size() * sizeof(T));
		}
	} // anonymous namespace

	void process_scene(scene& scene, const geometry_import_settings& settings, progression *const progression)
	{
		assert(progression);
		split_meshes_by_material(scene, progression);

		for(auto& lod : scene.lod_groups)
			for (auto& m : lod.meshes)
			{
				process_vertices(m, settings);
				progression->callback(progression->value() + 1, progression->max_value());
			}
	}

	void pack_data(const scene& scene, scene_data& data)
	{
		const u64 scene_size{ get_scene_size(scene) };
		data.buffer_size = (u32)scene_size;
#if defined(_MSC_VER)
		data.buffer = (u8*)CoTaskMemAlloc(scene_size);
#elif defined(__clang__)
		data.buffer = (u8*)malloc(scene_size);
#endif
		assert(data.buffer);

		utl::blob_stream_writer blob{ data.buffer, data.buffer_size };

		// scene name
		auto scene_name_size{ scene.name.size() };
		blob.write((u32)scene.name.size());
		blob.write(scene.name.c_str(), scene.name.size());
		// number of materials
		blob.write((u32)scene.materials.size());
		for (const auto& m : scene.materials)
		{
			blob.write((u32)m.name.size());
			blob.write(m.name.c_str(), m.name.size());
			blob.write((u32)m.diffuse_texture.size());
			blob.write(m.diffuse_texture.c_str(), m.diffuse_texture.size());
			blob.write((u32)m.normal_texture.size());
			blob.write(m.normal_texture.c_str(), m.normal_texture.size());
		}
		// number of LODS
		blob.write((u32)scene.lod_groups.size());

		for (const auto& lod : scene.lod_groups)
		{
			// LOD name
			blob.write((u32)lod.name.size());
			blob.write(lod.name.c_str(), lod.name.size());
			// number of meshes in this LOD
			blob.write((u32)lod.meshes.size());

			for (const auto& m : lod.meshes)
			{
				pack_mesh_data(m, blob);
			}
		}
		assert(scene_size == blob.offset());
	}

	bool coalesce_meshes(const lod_group& lod, mesh& combined_mesh, progression *const progression)
	{
		assert(lod.meshes.size());
		const mesh& first_mesh{ lod.meshes[0] };
		combined_mesh.name = first_mesh.name;
		combined_mesh.elements_type = determine_elements_type(first_mesh);
		combined_mesh.lod_threshold = first_mesh.lod_threshold;
		combined_mesh.lod_id = first_mesh.lod_id;
		combined_mesh.material_idx = first_mesh.material_idx;
		combined_mesh.uv_sets.resize(first_mesh.uv_sets.size());

		for (u32 mesh_idx{ 0 }; mesh_idx < lod.meshes.size(); ++mesh_idx)
		{
			const mesh& m{ lod.meshes[mesh_idx] };

			if (combined_mesh.elements_type != determine_elements_type(m) ||
				combined_mesh.uv_sets.size() != m.uv_sets.size() ||
				combined_mesh.lod_id != m.lod_id ||
				combined_mesh.material_idx != m.material_idx ||
				!math::is_equal(combined_mesh.lod_threshold, m.lod_threshold))
			{
				combined_mesh = {};
				return false;
			}
		}

		for (u32 mesh_idx{ 0 }; mesh_idx < lod.meshes.size(); ++mesh_idx)
		{
			const mesh& m{ lod.meshes[mesh_idx] };

			const u32 position_count{ (u32)combined_mesh.positions.size() };
			const u32 raw_index_base{ (u32)combined_mesh.raw_indices.size() };

			append_to_vector_pod(combined_mesh.positions, m.positions);
			append_to_vector_pod(combined_mesh.normals, m.normals);
			append_to_vector_pod(combined_mesh.tangents, m.tangents);
			append_to_vector_pod(combined_mesh.colors, m.colors);

			for (u32 i{ 0 }; i < combined_mesh.uv_sets.size(); ++i)
			{
				append_to_vector_pod(combined_mesh.uv_sets[i], m.uv_sets[i]);
			}

			append_to_vector_pod(combined_mesh.material_indices, m.material_indices);
			append_to_vector_pod(combined_mesh.raw_indices, m.raw_indices);

			for (u32 i{ raw_index_base }; i < combined_mesh.raw_indices.size(); ++i)
			{
				combined_mesh.raw_indices[i] += position_count;
			}

			progression->callback(progression->value(), progression->max_value() > 1 ? progression->max_value() - 1 : 1);
		}

		for (const u32 mtl_idx : combined_mesh.material_indices)
		{
			if (std::find(combined_mesh.material_used.begin(), combined_mesh.material_used.end(), mtl_idx) == combined_mesh.material_used.end())
			{
				combined_mesh.material_used.emplace_back(mtl_idx);
			}
		}

		return true;
	}
}
