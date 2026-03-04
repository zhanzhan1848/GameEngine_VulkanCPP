#pragma once
#include "CommonHeaders.h"

#include <limits>

namespace primal::graphics::utl
{
	constexpr float INF_FLOAT{ 3.4E38f };

	struct AABB
	{
		AABB() : min{ -INF_FLOAT, -INF_FLOAT, -INF_FLOAT }, max{ INF_FLOAT, INF_FLOAT, INF_FLOAT }, centroid{ 0.f, 0.f, 0.f } {}
		AABB(math::v3 LT, math::v3 RB) : min{ LT }, max{ RB }, centroid{ (LT.x + RB.x) * 0.5f, (LT.y + RB.y) * 0.5f, (LT.z + RB.z) * 0.5f } {}
		~AABB() {}

		AABB Union(const AABB& a) const
		{
			return { math::v3{std::min(this->min.x, a.min.x), std::min(this->min.y, a.min.y), std::min(this->min.z, a.min.z)},
					math::v3{std::max(this->max.x, a.max.x), std::max(this->max.y, a.max.y), std::max(this->max.z, a.max.z)} };
		}

		AABB Intersect(const AABB& a) const
		{
			return { math::v3{std::max(this->min.x, a.min.x), std::max(this->min.y, a.min.y), std::max(this->min.z, a.min.z)},
					math::v3{std::min(this->max.x, a.max.x), std::min(this->max.y, a.max.y), std::min(this->max.z, a.max.z)} };
		}

		bool Is_Intersection(const AABB& a, const AABB& b)
		{
			// Note: Calculate Two Bounding Boxes is collection, by SAT function
			//		 Base of two bounding boxes, so one of the surfaces' normal will only in x, y, z axis
			//		 So we only check is intersect in x, y, z axis
			return (a.min.x <= b.max.x && a.max.x >= b.min.x) &&
				(a.min.y <= b.max.y && a.max.y >= b.min.y) &&
				(a.min.z <= b.max.z && a.max.z >= b.min.z);
		}

		math::v3			min, max, centroid;
	};

	class BVH
	{
	public:
		struct Ray
		{
			math::v3 origin;
			math::v3 direction;
			math::v3 inv_direction;
			float t_min{ 0.001f };
			float t_max{ INF_FLOAT };

			Ray(math::v3 o, math::v3 d) : origin(o), direction(d)
			{
				inv_direction = { 1.0f / d.x, 1.0f / d.y, 1.0f / d.z };
			}
		};

		struct HitInfo
		{
			float t{ INF_FLOAT };
			float u{ 0.f };
			float v{ 0.f };
			u32 triangle_index{ u32_invalid_id };
			bool hit{ false };
		};

		BVH() = default;

		void Build(const primal::utl::vector<math::v3>& vertices, const primal::utl::vector<u32>& indices);
		bool Intersect(const Ray& ray, HitInfo& out_hit) const;
        // 快速遮挡检测 (Any Hit)
        bool IntersectAny(const Ray& ray, float max_dist) const;

	private:
		struct BVHNode
		{
			u32 left{ u32_invalid_id };   // Left child index or invalid
			u32 right{ u32_invalid_id };  // Right child index or invalid
			u32 first_primitive{ u32_invalid_id }; // Index into m_primitive_indices
			u32 primitive_count{ 0 };
			AABB bbox;

			bool IsLeaf() const { return primitive_count > 0; }
		};

		struct Triangle
		{
			math::v3 v0, v1, v2;
			math::v3 center;
            u32 original_index; // Index in the input index buffer / 3
		};

		primal::utl::vector<BVHNode> m_nodes;
		primal::utl::vector<Triangle> m_triangles;
        primal::utl::vector<u32> m_primitive_indices; // Indirection to m_triangles

		void UpdateNodeBounds(u32 node_index);
		void Subdivide(u32 node_index);
		bool IntersectTriangle(const Ray& ray, const Triangle& tri, HitInfo& hit) const;
		bool IntersectAABB(const Ray& ray, const AABB& aabb, float& t_enter) const;
	};
}