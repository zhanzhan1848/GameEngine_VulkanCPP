namespace PrimalEditor_Avalonia.Native.MaterialGraph;

/// <summary>
/// Mirrors <c>MaterialParamType</c> in
/// <c>Engine/Graphics/MaterialGraph/MaterialGraphReflection.h</c>. Numeric
/// values must match the C++ side — the ABI passes the raw <c>u8</c>
/// through.
/// </summary>
public enum MaterialParamType : uint
{
    Bool = 0,
    Float = 1,
    Float2 = 2,
    Float3 = 3,
    Float4 = 4,
    Int = 5,
    UInt = 6,
    Enum = 7,
    Texture = 8,
    Curve = 9,
}