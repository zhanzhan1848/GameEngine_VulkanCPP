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
    public const float EditorVerticalFieldOfViewDegrees = 60f;

    /// <summary>
    /// Pixel focal length matching the native editor camera's vertical FOV.
    /// Perspective projection is based on viewport height, not the smaller
    /// viewport dimension; using min(width,height)/2 implicitly selected a
    /// 90-degree FOV and displaced managed gizmo hit tests from Metal output.
    /// </summary>
    public static float PerspectiveFocalLength(
        float viewportHeight,
        float verticalFieldOfViewDegrees = EditorVerticalFieldOfViewDegrees)
    {
        if (viewportHeight <= 0f) return 0f;
        var halfFovRadians = verticalFieldOfViewDegrees * MathF.PI / 360f;
        return viewportHeight * 0.5f / MathF.Tan(halfFovRadians);
    }

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
        // The native transform system rotates the entity's local +Z axis to
        // obtain its forward direction. Keep the editor orbit camera on that
        // same convention so rendering, overlays and picking agree.
        fx = syA * cpA;
        fy = spA;
        fz = cyA * cpA;
        // Camera position.
        camX = targetX - fx * distance;
        camY = targetY - fy * distance;
        camZ = targetZ - fz * distance;
        // Right = normalize(worldUp × forward). At yaw=0 this is +X.
        rx = fz;
        ry = 0f;
        rz = -fx;
        float rlen = MathF.Sqrt(rx * rx + ry * ry + rz * rz);
        if (rlen > 1e-6f) { rx /= rlen; ry /= rlen; rz /= rlen; }
        // Up = forward × right.
        ux = fy * rz - fz * ry;
        uy = fz * rx - fx * rz;
        uz = fx * ry - fy * rx;
    }

    /// <summary>
    /// Convert editor orbit state into the position and XYZ Euler rotation
    /// expected by SetEntityTransform. The native transform's local +Z axis
    /// points from the resulting camera position back to the orbit target.
    /// </summary>
    public static void ComputeCameraTransform(
        float targetX, float targetY, float targetZ,
        float yaw, float pitch, float distance,
        out float posX, out float posY, out float posZ,
        out float rotX, out float rotY, out float rotZ)
    {
        ComputeCameraBasis(
            targetX, targetY, targetZ, yaw, pitch, distance,
            out posX, out posY, out posZ,
            out _, out _, out _,
            out _, out _, out _,
            out _, out _, out _);
        rotX = -pitch;
        rotY = yaw;
        rotZ = 0f;
    }
}
