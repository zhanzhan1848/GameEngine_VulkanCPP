#pragma once

#include "SimulationAPI.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace primal::simulation {

// body_desc → Jolt 形状。仅支持 box/sphere（M0）；capsule/convex_hull/triangle_mesh 在 M1+ 扩展。
// 失败（未知形状/非法参数）返回 nullptr，由调用方记日志并让 body_create 失败。
JPH::ShapeRefC make_body_shape(const body_desc& d);

} // namespace primal::simulation
