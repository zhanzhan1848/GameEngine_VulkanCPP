using System;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// Pure-math world → screen projector used by the gizmo and selection
/// overlays. Extracted from <see cref="ViewportOverlay"/> so it can be
/// unit-tested independently of any UI framework. Camera convention
/// matches <see cref="EngineViewport3D"/>: orbit camera around a
/// target, yaw rotates around world +Y, pitch tilts forward axis up
/// or down.
/// </summary>
public static class WorldProjector
{
    /// <summary>
    /// Cached camera basis: world-space camera position + right/up/forward
    /// unit vectors. Computed once per frame from the editor orbit
    /// camera state and reused by both the gizmo overlay and the icon
    /// renderer so they stay in lockstep with the rendered scene.
    /// </summary>
    public sealed class Basis
    {
        public float CamX, CamY, CamZ;
        public float RightX, RightY, RightZ;
        public float UpX, UpY, UpZ;
        public float ForwardX, ForwardY, ForwardZ;
    }

    /// <summary>
    /// Convenience overload of <see cref="ComputeCameraBasis(float,float,float,float,float,float,out float,out float,out float,out float,out float,out float,out float,out float,out float)"/>
    /// that fills a reusable <see cref="Basis"/> struct instead of
    /// 12 out parameters.
    /// </summary>
    public static void ComputeCameraBasis(
        float targetX, float targetY, float targetZ,
        float yaw, float pitch, float distance,
        Basis basis)
    {
        ComputeCameraBasis(
            targetX: targetX, targetY: targetY, targetZ: targetZ,
            yaw: yaw, pitch: pitch, distance: distance,
            out basis.CamX, out basis.CamY, out basis.CamZ,
            out basis.RightX, out basis.RightY, out basis.RightZ,
            out basis.UpX, out basis.UpY, out basis.UpZ,
            out basis.ForwardX, out basis.ForwardY, out basis.ForwardZ);
    }
    /// <summary>
    /// Project a world-space point to viewport pixel coords. Returns
    /// null if the point is at or behind the camera's near plane
    /// (cw &lt;= <paramref name="nearEpsilon"/>).
    /// </summary>
    public static (float sx, float sy)? Project(
        float px, float py, float pz,
        float camX, float camY, float camZ,
        float rx, float ry, float rz,
        float ux, float uy, float uz,
        float fx, float fy, float fz,
        float viewportW, float viewportH,
        float focal,
        float nearEpsilon = 0.05f)
    {
        float dx = px - camX, dy = py - camY, dz = pz - camZ;
        float cu = dx * rx + dy * ry + dz * rz;
        float cv = dx * ux + dy * uy + dz * uz;
        float cw = dx * fx + dy * fy + dz * fz;
        if (cw < nearEpsilon) return null;
        return (viewportW * 0.5f + (cu / cw) * focal, viewportH * 0.5f - (cv / cw) * focal);
    }

    /// <summary>
    /// Compute the orbit camera basis from a target + yaw + pitch +
    /// distance. Returns (cam, right, up, forward).
    /// </summary>
    public static void ComputeCameraBasis(
        float targetX, float targetY, float targetZ,
        float yaw, float pitch, float distance,
        out float camX, out float camY, out float camZ,
        out float rx, out float ry, out float rz,
        out float ux, out float uy, out float uz,
        out float fx, out float fy, out float fz)
    {
        float cyA = MathF.Cos(yaw), syA = MathF.Sin(yaw);
        float cpA = MathF.Cos(pitch), spA = MathF.Sin(pitch);
        // Camera forward (yaw=0 → -Z).
        fx = -syA * cpA;
        fy = -spA;
        fz = -cyA * cpA;
        // Camera position.
        camX = targetX - fx * distance;
        camY = targetY - fy * distance;
        camZ = targetZ - fz * distance;
        // Right = normalize(forward × worldUp).
        float wUpX = 0f, wUpY = 1f, wUpZ = 0f;
        rx = fy * wUpZ - fz * wUpY;
        ry = fz * wUpX - fx * wUpZ;
        rz = fx * wUpY - fy * wUpX;
        float rlen = MathF.Sqrt(rx * rx + ry * ry + rz * rz);
        if (rlen > 1e-6f) { rx /= rlen; ry /= rlen; rz /= rlen; }
        // Up = right × forward.
        ux = ry * fz - rz * fy;
        uy = rz * fx - rx * fz;
        uz = rx * fy - ry * fx;
    }
}