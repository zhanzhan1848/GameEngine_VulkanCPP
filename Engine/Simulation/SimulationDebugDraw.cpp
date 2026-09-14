#include "SimulationDebugDraw.h"

#ifdef JPH_DEBUG_RENDERER

// 本 TU 以 -fno-rtti 编译（Jolt 基类无 typeinfo），不能包含 CommonHeaders 链
// （Utilities/FreeList.h 用 typeid）。引擎类型一律用 Jolt 同尺寸类型；
// debug_draw::add_line 以手写原型解耦——签名变更时必须同步
// Graphics/DebugDraw/DebugDrawQueue.h。
namespace primal::graphics::debug_draw {
void add_line(float x0, float y0, float z0, float x1, float y1, float z1, unsigned int rgb);
}

using primal::graphics::debug_draw::add_line;

namespace primal::simulation {

using graphics::debug_draw::add_line;

void simulation_debug_renderer::DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor)
{
    add_line(inFrom.GetX(), inFrom.GetY(), inFrom.GetZ(),
             inTo.GetX(),   inTo.GetY(),   inTo.GetZ(),
             (static_cast<unsigned int>(inColor.b) << 16) | (static_cast<unsigned int>(inColor.g) << 8) | static_cast<unsigned int>(inColor.r));
    ++_lines_forwarded;
}

} // namespace primal::simulation

#endif // JPH_DEBUG_RENDERER
