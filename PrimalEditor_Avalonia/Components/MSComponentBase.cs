using System;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Helpers shared by every <see cref="MSComponent{T}"/> subclass:
/// nullable T-valued setter that no-ops when the underlying
/// component value didn't change (within 1e-4), so multi-selection
/// keeps the "mixed values" sentinel while the user types.
/// </summary>
public abstract class MSComponentBase<T> : MSComponent<T> where T : Component
{
    protected MSComponentBase(MSEntity msEntity) : base(msEntity) { }

    /// <summary>
    /// Compare two nullable floats. Null vs null → equal; null vs
    /// value → not equal; tiny differences within epsilon → equal.
    /// </summary>
    protected static bool Same(float? a, float? b)
    {
        if (a == null && b == null) return true;
        if (a == null || b == null) return false;
        return Math.Abs(a.Value - b.Value) < 1e-4f;
    }
}
