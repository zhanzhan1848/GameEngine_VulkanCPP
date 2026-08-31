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
    private float _cameraDistance = 12f;

    /// <summary>
    /// Keep the editor camera basis available for future billboard-style
    /// helpers. Transform-aware Light and Camera wire icons are world-space.
    /// </summary>
    public void UpdateCamera(float yaw, float pitch, float distance,
        float targetX = 0f, float targetY = 0f, float targetZ = 0f)
    {
        _cameraDistance = distance;
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
    public void DrawEditorVisuals(
        Scene? scene,
        bool gizmoVisible,
        (float x, float y, float z) gizmoTarget,
        GizmoOverlay.GizmoMode gizmoMode)
    {
        NativeEngineBridge.ClearDebugLines();

        DrawGridAndWorldAxes();
        if (scene != null) DrawSceneIconGeometry(scene);
        if (gizmoVisible) DrawGizmo(gizmoTarget, gizmoMode);
    }

    /// <summary>Compatibility entry point used by focused icon tests and
    /// callers that only want scene icons, without the grid or gizmo.</summary>
    public void DrawSceneIcons(Scene? scene)
    {
        NativeEngineBridge.ClearDebugLines();
        if (scene != null) DrawSceneIconGeometry(scene);
    }

    private static void Line(
        (float x, float y, float z) a,
        (float x, float y, float z) b,
        byte r, byte g, byte blue)
    {
        NativeEngineBridge.DrawDebugLine(a.x, a.y, a.z, b.x, b.y, b.z, r, g, blue);
    }

    private static void DrawGridAndWorldAxes()
    {
        const int extent = 20;
        for (var i = -extent; i <= extent; ++i)
        {
            if (i != 0)
            {
                var major = i % 5 == 0;
                var c = major ? (byte)86 : (byte)58;
                Line((i, 0, -extent), (i, 0, extent), c, c, (byte)(c + 8));
                Line((-extent, 0, i), (extent, 0, i), c, c, (byte)(c + 8));
            }
        }

        // World axes remain longer than the small origin indicator so their
        // orientation is obvious even when the camera is far away.
        Line((-extent, 0, 0), (extent, 0, 0), 220, 72, 72);
        Line((0, 0, -extent), (0, 0, extent), 72, 112, 235);
        Line((0, 0, 0), (0, 4, 0), 72, 220, 92);
    }

    private void DrawGizmo(
        (float x, float y, float z) target,
        GizmoOverlay.GizmoMode mode)
    {
        var len = System.Math.Clamp(_cameraDistance * 0.12f, 0.65f, 4f);
        var head = len * 0.18f;

        void Axis((float x, float y, float z) end, byte r, byte g, byte b)
        {
            Line(target, end, r, g, b);
        }

        if (mode == GizmoOverlay.GizmoMode.Rotate)
        {
            const int segments = 48;
            void Ring(int plane, byte r, byte g, byte b)
            {
                for (var i = 0; i < segments; ++i)
                {
                    var a0 = System.MathF.Tau * i / segments;
                    var a1 = System.MathF.Tau * (i + 1) / segments;
                    (float x, float y, float z) P(float a) => plane switch
                    {
                        0 => (target.x, target.y + System.MathF.Cos(a) * len, target.z + System.MathF.Sin(a) * len),
                        1 => (target.x + System.MathF.Cos(a) * len, target.y, target.z + System.MathF.Sin(a) * len),
                        _ => (target.x + System.MathF.Cos(a) * len, target.y + System.MathF.Sin(a) * len, target.z),
                    };
                    Line(P(a0), P(a1), r, g, b);
                }
            }
            Ring(0, 240, 70, 70);
            Ring(1, 70, 235, 90);
            Ring(2, 70, 125, 255);
            return;
        }

        var xEnd = (target.x + len, target.y, target.z);
        var yEnd = (target.x, target.y + len, target.z);
        var zEnd = (target.x, target.y, target.z + len);
        Axis(xEnd, 240, 70, 70);
        Axis(yEnd, 70, 235, 90);
        Axis(zEnd, 70, 125, 255);

        if (mode == GizmoOverlay.GizmoMode.Translate)
        {
            // Arrowheads lie in two planes so they remain legible from most
            // editor-camera angles.
            Line(xEnd, (xEnd.Item1 - head, xEnd.Item2 + head * .45f, xEnd.Item3), 240, 70, 70);
            Line(xEnd, (xEnd.Item1 - head, xEnd.Item2, xEnd.Item3 + head * .45f), 240, 70, 70);
            Line(yEnd, (yEnd.Item1 + head * .45f, yEnd.Item2 - head, yEnd.Item3), 70, 235, 90);
            Line(yEnd, (yEnd.Item1, yEnd.Item2 - head, yEnd.Item3 + head * .45f), 70, 235, 90);
            Line(zEnd, (zEnd.Item1 + head * .45f, zEnd.Item2, zEnd.Item3 - head), 70, 125, 255);
            Line(zEnd, (zEnd.Item1, zEnd.Item2 + head * .45f, zEnd.Item3 - head), 70, 125, 255);
        }
        else
        {
            void Cube((float x, float y, float z) c, byte r, byte g, byte b)
            {
                var s = head * .45f;
                var p = new[]
                {
                    (c.x-s,c.y-s,c.z-s), (c.x+s,c.y-s,c.z-s),
                    (c.x+s,c.y+s,c.z-s), (c.x-s,c.y+s,c.z-s),
                    (c.x-s,c.y-s,c.z+s), (c.x+s,c.y-s,c.z+s),
                    (c.x+s,c.y+s,c.z+s), (c.x-s,c.y+s,c.z+s),
                };
                int[,] edges = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7} };
                for (var i = 0; i < 12; ++i) Line(p[edges[i,0]], p[edges[i,1]], r, g, b);
            }
            Cube(xEnd, 240, 70, 70);
            Cube(yEnd, 70, 235, 90);
            Cube(zEnd, 70, 125, 255);
        }
    }

    private static void DrawSceneIconGeometry(Scene scene)
    {

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
