// PhysicsQuery.cpp — 见 PhysicsQuery.h。

#include "PhysicsQuery.h"
#include "Simulation/SimulationAPI.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::interop {

namespace {

bool sim_raycast(const math::v3& from, const math::v3& dir, f32 max_dist, raycast_hit& out, void* /*user*/) {
    math::v3 point, normal;
    if (!simulation::raycast(from, dir, max_dist, point, normal)) return false;
    out.point = point;
    out.normal = normal;
    const f32 dx = point.x - from.x, dy = point.y - from.y, dz = point.z - from.z;
    out.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return true;
}

} // namespace

void install_simulation_adapter() { physics_query::install(sim_raycast, nullptr); }

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
