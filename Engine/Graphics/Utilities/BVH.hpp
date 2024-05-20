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

		AABB Union(const AABB& a)
		{
			return { {std::min(this->min.x, a.min.x), std::min(this->min.y, a.min.y), std::min(this->min.z, a.min.z)},
					{std::max(this->max.x, a.max.x), std::max(this->max.y, a.max.y), std::max(this->max.z, a.max.z)} };
		}

		AABB Intersection(const AABB& a)
		{
			return { {std::max(this->min.x, a.min.x), std::max(this->min.y, a.min.y), std::max(this->min.z, a.min.z)},
					{std::min(this->max.x, a.max.x), std::min(this->max.y, a.max.y), std::min(this->max.z, a.max.z)} };
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
		BVH() = default;

	private:
		struct BVHNode
		{
			u32					left, right;
			u32					n, index;
			AABB				bbox;
		};
	};
}