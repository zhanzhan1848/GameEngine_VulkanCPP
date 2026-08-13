using System;
using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>Pure transform math shared by viewport scene icons and tests.</summary>
public static class SceneIconGeometry
{
    /// <summary>
    /// Applies entity scale, then its Z * Y * X Euler rotation (radians),
    /// then translation to an icon vertex expressed in entity-local space.
    /// This is the same Euler convention used by EntityAPI.cpp.
    /// </summary>
    public static (float x, float y, float z) TransformPoint(
        Transform transform, float x, float y, float z)
    {
        x *= transform.ScaleX;
        y *= transform.ScaleY;
        z *= transform.ScaleZ;

        var cx = MathF.Cos(transform.RotationX);
        var sx = MathF.Sin(transform.RotationX);
        (y, z) = (y * cx - z * sx, y * sx + z * cx);

        var cy = MathF.Cos(transform.RotationY);
        var sy = MathF.Sin(transform.RotationY);
        (x, z) = (x * cy + z * sy, -x * sy + z * cy);

        var cz = MathF.Cos(transform.RotationZ);
        var sz = MathF.Sin(transform.RotationZ);
        (x, y) = (x * cz - y * sz, x * sz + y * cz);

        return (x + transform.PositionX, y + transform.PositionY, z + transform.PositionZ);
    }
}
