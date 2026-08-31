namespace PrimalEditor_Avalonia.Native.MaterialGraph;

/// <summary>
/// Mirrors <c>PreviewModelType</c> in
/// <c>Engine/Graphics/MaterialPreview/MaterialPreviewRenderer.h</c>. Numeric
/// values must match the C++ side exactly — they're passed directly to
/// <see cref="MaterialPreviewNative.SetModel"/> as a u32.
/// </summary>
public enum PreviewModelType
{
    Sphere = 0,
    Box = 1,
    Cylinder = 2,
    Cone = 3,
    Torus = 4,
    Capsule = 5,
    Teapot = 6,
    Plane = 7,
}