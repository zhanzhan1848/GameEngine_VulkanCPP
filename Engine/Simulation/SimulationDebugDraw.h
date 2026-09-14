#pragma once

// SimulationDebugDraw.h — Jolt DebugRenderer → 引擎 debug_draw 队列的桥。
// 只转发线段（box/sphere 的线框 proxy 足够）；三角形与 3D 文本丢弃。
// 线段由 ForwardSceneRenderer 的 Pass 6（LineBatchRenderer）渲染，零渲染管线改动。
//
// Jolt 的 DebugRenderer 只在 JPH_DEBUG_RENDERER 下编译（Debug 配置默认带，
// 见根 CMakeLists 的 DEBUG_RENDERER_IN_DEBUG_AND_RELEASE）；其余配置下本文件
// 提供空实现，debug_draw_bodies() 恒返回 0（可视化测试固定跑 Debug）。
//
// 注意：本 TU 以 -fno-rtti 编译（Engine/CMakeLists.txt 中设置——Jolt 基类无
// typeinfo），因此这里不能包含带 typeid 的引擎头（如 GameEntity 链上的 FreeList）。

#ifdef JPH_DEBUG_RENDERER

#include <Jolt/Jolt.h>
#include <Jolt/Renderer/DebugRenderer.h>

namespace primal::simulation {

class simulation_debug_renderer final : public JPH::DebugRenderer
{
public:
    simulation_debug_renderer() { Initialize(); }

    JPH::uint64 lines_forwarded() const { return _lines_forwarded; }

protected:
    void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override;
    void DrawTriangle(JPH::RVec3Arg, JPH::RVec3Arg, JPH::RVec3Arg, JPH::ColorArg,
                      JPH::DebugRenderer::ECastShadow) override {}
    // 线框 proxy 不需要几何上传：三角批次返回空句柄，DrawGeometry/Text3D 丢弃。
    Batch CreateTriangleBatch(const JPH::DebugRenderer::Triangle*, int) override { return Batch(); }
    Batch CreateTriangleBatch(const JPH::DebugRenderer::Vertex*, int,
                              const JPH::uint32*, int) override { return Batch(); }
    void DrawGeometry(JPH::RMat44Arg, const JPH::AABox&, float, JPH::ColorArg,
                      const JPH::DebugRenderer::GeometryRef&, JPH::DebugRenderer::ECullMode,
                      JPH::DebugRenderer::ECastShadow, JPH::DebugRenderer::EDrawMode) override {}
    void DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) override {}

private:
    JPH::uint64 _lines_forwarded{ 0 };
};

} // namespace primal::simulation

#endif // JPH_DEBUG_RENDERER
