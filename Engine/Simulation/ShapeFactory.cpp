#include "ShapeFactory.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

namespace primal::simulation {

JPH::ShapeRefC make_body_shape(const body_desc& d)
{
    using S = body_desc::shape;
    switch (d.s) {
        case S::box:
            return new JPH::BoxShape(
                JPH::Vec3{ d.half_extents.x, d.half_extents.y, d.half_extents.z });
        case S::sphere:
            return new JPH::SphereShape(d.half_extents.x);
    }
    return nullptr;
}

} // namespace primal::simulation
