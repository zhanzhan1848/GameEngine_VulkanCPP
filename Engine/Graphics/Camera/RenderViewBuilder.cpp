#include "RenderViewBuilder.h"
#include "EngineAPI/Camera.h"
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics {

RenderView BuildRenderViewFromCameraId(camera_id id, const rhi::ViewportDesc& viewport) {
    RenderView view;

    // Always honor the caller's viewport, even on the invalid-id early-out
    // path — the viewport is independent of the camera, and a default-constructed
    // RenderView has an empty viewport (0×0) which is never what the caller wants.
    view.SetViewport(viewport);

    // Default-constructed RenderView has identity matrices. Early-out on invalid
    // id leaves view/projection as identity.
    if (!id::is_valid(id)) return view;

    // Use the public camera class which internally dispatches to the registered
    // platform backend (Metal/Vulkan/D3D12) via the graphics platform interface.
    // We construct a camera wrapper from the id and ask it for the current
    // view/projection matrices. The camera class asserts is_valid() internally.
    camera cam{id};

    // Refresh the camera's view matrix from the camera entity's current
    // transform. Without this, the platform camera (MetalCamera) keeps the
    // view matrix it was constructed with, so any subsequent transform
    // changes (e.g. editor orbit/pan/zoom via SetEntityTransform) are
    // silently ignored — the rendered scene stays anchored to the camera's
    // initial pose. The legacy `end_frame` path calls camera.update() in
    // get_metal_frame_info; the StandardRenderPipeline path (Path B) goes
    // through this function and would otherwise miss the refresh.
    cam.update();

    view.SetType(ViewType::Main);
    view.SetViewMatrix(cam.view());
    view.SetProjectionMatrix(cam.projection());
    // RenderView::SetViewMatrix/SetProjectionMatrix auto-derive viewProjection
    // via UpdateFrustum(); no separate setter exists or is needed.

    // [PassDebugView 排查] 一次性打印相机原始 view/projection（列主序）与
    // 编辑器实际收到的 RenderView 矩阵，用于定位矩阵在何处损坏/换约定。
    // 实测发现 RG 路径 view 矩阵旋转与平移不自洽（[CullProbe] 全拒根因）。
    {
        static bool s_dumped = false;
        if (!s_dumped) {
            s_dumped = true;
            const math::m4x4 cv = cam.view();
            const math::m4x4 cp = cam.projection();
            const math::m4x4 rv = view.GetViewMatrix();
            const math::m4x4 rp = view.GetProjectionMatrix();
            auto mToStr = [](const math::m4x4& m) {
                std::string s;
                for (int c = 0; c < 4; ++c) {
                    const math::v4 col = math::column(m, c);
                    s += " c" + std::to_string(c) + "=(" +
                         std::to_string(col.x) + "," +
                         std::to_string(col.y) + "," +
                         std::to_string(col.z) + "," +
                         std::to_string(col.w) + ")";
                }
                return s;
            };
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[CamDump] cam.view:" << mToStr(cv) << std::endl;
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[CamDump] cam.proj:" << mToStr(cp) << std::endl;
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[CamDump] renderView.view:" << mToStr(rv) << std::endl;
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[CamDump] renderView.proj:" << mToStr(rp) << std::endl;
        }
    }

    return view;
}

} // namespace primal::graphics
