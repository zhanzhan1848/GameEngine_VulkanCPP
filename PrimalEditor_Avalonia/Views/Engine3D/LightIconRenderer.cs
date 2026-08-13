using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// Editor-side icon overlay rendered into the engine framebuffer
/// (not into Avalonia's 2D tree), so the helpers stay visible above the
/// native Metal surface on macOS.
///
/// Draws transform-aware wire icons for Light and Camera components.
/// </summary>
public sealed class LightIconRenderer
{
    private readonly WorldProjector.Basis _cameraBasis = new();

    /// <summary>
    /// Keep the editor camera basis available for future billboard-style
    /// helpers. Transform-aware Light and Camera wire icons are world-space.
    /// </summary>
    public void UpdateCamera(float yaw, float pitch, float distance,
        float targetX = 0f, float targetY = 0f, float targetZ = 0f)
    {
        WorldProjector.ComputeCameraBasis(
            targetX: targetX, targetY: targetY, targetZ: targetZ,
            yaw: yaw, pitch: pitch, distance: distance,
            _cameraBasis);
    }

    /// <summary>
    /// Draw all Light and Camera icon glyphs for the active scene. Safe to
    /// call every frame; the C ABI is no-op when the engine isn't
    /// loaded.
    /// </summary>
    public void DrawSceneIcons(Scene? scene)
    {
        if (scene == null) return;
        NativeEngineBridge.ClearDebugLines();

        void Segment(Transform transform,
            (float x, float y, float z) a,
            (float x, float y, float z) b,
            byte r, byte g, byte blue)
        {
            var wa = SceneIconGeometry.TransformPoint(transform, a.x, a.y, a.z);
            var wb = SceneIconGeometry.TransformPoint(transform, b.x, b.y, b.z);
            NativeEngineBridge.DrawDebugLine(
                wa.x, wa.y, wa.z, wb.x, wb.y, wb.z, r, g, blue);
        }

        void DrawLight(Transform transform)
        {
            const int sides = 12;
            for (var i = 0; i < sides; i++)
            {
                var a = i * System.MathF.Tau / sides;
                var b = (i + 1) * System.MathF.Tau / sides;
                Segment(transform,
                    (System.MathF.Cos(a) * 0.22f, System.MathF.Sin(a) * 0.22f, 0),
                    (System.MathF.Cos(b) * 0.22f, System.MathF.Sin(b) * 0.22f, 0),
                    255, 214, 64);
                if ((i & 1) == 0)
                    Segment(transform,
                        (System.MathF.Cos(a) * 0.31f, System.MathF.Sin(a) * 0.31f, 0),
                        (System.MathF.Cos(a) * 0.48f, System.MathF.Sin(a) * 0.48f, 0),
                        255, 214, 64);
            }
        }

        void DrawCamera(Transform transform)
        {
            var near = new[]
            {
                (-0.28f, -0.20f, 0.10f), (0.28f, -0.20f, 0.10f),
                (0.28f, 0.20f, 0.10f), (-0.28f, 0.20f, 0.10f),
            };
            var far = new[]
            {
                (-0.48f, -0.34f, -0.72f), (0.48f, -0.34f, -0.72f),
                (0.48f, 0.34f, -0.72f), (-0.48f, 0.34f, -0.72f),
            };
            for (var i = 0; i < 4; i++)
            {
                Segment(transform, near[i], near[(i + 1) % 4], 78, 205, 255);
                Segment(transform, far[i], far[(i + 1) % 4], 78, 205, 255);
                Segment(transform, near[i], far[i], 78, 205, 255);
            }
            Segment(transform, (-0.16f, 0.20f, 0.10f), (0, 0.38f, 0.10f), 78, 205, 255);
            Segment(transform, (0, 0.38f, 0.10f), (0.16f, 0.20f, 0.10f), 78, 205, 255);
        }

        void Visit(GameEntity entity)
        {
            if (!entity.IsEnabled) return;
            var transform = entity.GetTransform();
            if (transform != null)
            {
                if (entity.GetComponent<LightComponent>() != null) DrawLight(transform);
                if (entity.GetComponent<CameraComponent>() != null) DrawCamera(transform);
            }
            foreach (var child in entity.Children) Visit(child);
        }

        foreach (var entity in scene.Entities) Visit(entity);
    }
}
