using System.Collections.Generic;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// Editor-side icon overlay rendered into the engine framebuffer
/// (not into Avalonia's 2D tree). The editor owns the camera math, so
/// it computes the billboard corners itself and asks the engine to
/// draw the world-space quads via the debug-draw C ABI — keeping the
/// icons in lockstep with the rendered scene.
///
/// Currently draws one icon per <see cref="LightComponent"/> in the
/// active scene. The icon is "always face the camera" by anchoring
/// the quad on a camera-facing basis computed from the editor orbit
/// camera state (same math as GizmoOverlay's projection).
/// </summary>
public sealed class LightIconRenderer
{
    private readonly WorldProjector.Basis _cameraBasis = new();

    /// <summary>
    /// Recompute the camera basis. Call once per frame before
    /// <see cref="DrawSceneIcons"/>; the editor's per-frame layout
    /// (orbit yaw/pitch/distance) feeds the basis used to project the
    /// billboards. We don't pull the camera state ourselves so the
    /// engine viewport remains the single source of truth.
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
    /// Draw all icon glyphs for the active scene's lights. Safe to
    /// call every frame; the C ABI is no-op when the engine isn't
    /// loaded.
    /// </summary>
    public void DrawSceneIcons(Scene? scene)
    {
        if (scene == null) return;

        foreach (var entity in scene.Entities)
        {
            var light = entity.GetComponent<LightComponent>();
            if (light == null) continue;
            var t = entity.GetTransform();
            if (t == null) continue;

            // Billboard quad. Sized in world units so the icon doesn't
            // shrink when the camera dollies in. 0.6u square looks like
            // a small flag at the light's position.
            DrawBillboard(t.PositionX, t.PositionY, t.PositionZ, 0.6f, 0.6f, 255, 220, 80);

            // Light-bulb: short rays make the light directionality obvious.
            // Directional lights have a natural axis to draw along;
            // point/spot lights just radiate outward.
            if (light.LightType == LightType.Directional)
            {
                NativeEngineBridge.DrawDebugLightBulb(
                    t.PositionX, t.PositionY, t.PositionZ,
                    radius: 0.05f, rayLength: 0.4f, 255, 220, 80);
            }
            else
            {
                NativeEngineBridge.DrawDebugLightBulb(
                    t.PositionX, t.PositionY, t.PositionZ,
                    radius: 0.18f, rayLength: 0.35f, 255, 220, 80);
            }
        }
    }

    /// <summary>
    /// Draw a flat quad facing the camera at the world-space center.
    /// The quad lives in world space but its orientation tracks the
    /// editor camera each frame.
    /// </summary>
    private void DrawBillboard(
        float cx, float cy, float cz,
        float halfW, float halfH,
        byte r, byte g, byte b)
    {
        var basis = _cameraBasis;
        float x0 = cx + (-halfW) * basis.RightX + (-halfH) * basis.UpX;
        float y0 = cy + (-halfW) * basis.RightY + (-halfH) * basis.UpY;
        float z0 = cz + (-halfW) * basis.RightZ + (-halfH) * basis.UpZ;
        float x1 = cx + (+halfW) * basis.RightX + (-halfH) * basis.UpX;
        float y1 = cy + (+halfW) * basis.RightY + (-halfH) * basis.UpY;
        float z1 = cz + (+halfW) * basis.RightZ + (-halfH) * basis.UpZ;
        float x2 = cx + (+halfW) * basis.RightX + (+halfH) * basis.UpX;
        float y2 = cy + (+halfW) * basis.RightY + (+halfH) * basis.UpY;
        float z2 = cz + (+halfW) * basis.RightZ + (+halfH) * basis.UpZ;
        float x3 = cx + (-halfW) * basis.RightX + (+halfH) * basis.UpX;
        float y3 = cy + (-halfW) * basis.RightY + (+halfH) * basis.UpY;
        float z3 = cz + (-halfW) * basis.RightZ + (+halfH) * basis.UpZ;

        NativeEngineBridge.DrawDebugWorldQuad(
            x0, y0, z0, x1, y1, z1, x2, y2, z2, x3, y3, z3, r, g, b);
    }
}