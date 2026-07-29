namespace PrimalEditor_Avalonia.Common;

public interface IDirtyTrackable
{
    bool IsDirty { get; }
    void MarkDirty();
    void ClearDirty();
}
