using System;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Editors;

public enum AssetEditorState
{
    Done,
    Importing,
    Processing,
    Loading,
    Saving,
}

public interface IAssetEditor
{
    AssetEditorState State { get; }
    Guid AssetGuid { get; }
    AssetInfo? Asset { get; }
    string Title { get; }
    /// <summary>True when the editor's settings are unsaved.</summary>
    bool IsDirty { get; }
    /// <summary>Persist the current settings to the asset's import sidecar.</summary>
    void Save();
    /// <summary>Reload settings from the asset's import sidecar, discarding in-memory edits.</summary>
    void Discard();
    void SetAsset(AssetInfo info);
}

public abstract class AssetEditorViewModel : ViewModelBase, IAssetEditor, Common.IDirtyTrackable
{
    private AssetEditorState _state = AssetEditorState.Done;
    public AssetEditorState State
    {
        get => _state;
        set { _state = value; OnPropertyChanged(); }
    }

    public Guid AssetGuid { get; private set; }
    public AssetInfo? Asset { get; private set; }
    public virtual string Title => Asset?.FileName ?? "(no asset)";

    /// <summary>
    /// Per-instance Undo/Redo stack. Subclasses that expose mutating
    /// settings call <see cref="RecordChange"/> from their setters so
    /// the user can undo edits with Ctrl+Z inside the asset editor.
    /// </summary>
    public Common.UndoRedo UndoRedo { get; } = new Common.UndoRedo();

    /// <summary>
    /// Push a one-shot undoable action onto the editor's UndoRedo. The
    /// caller is responsible for any state mutation that needs to be
    /// undone; this method only registers the action.
    /// </summary>
    protected void RecordChange(Common.IUndoRedoAction action)
    {
        if (action == null) return;
        UndoRedo.Add(action);
        MarkDirty();
    }

    /// <summary>Default: not dirty. Subclasses with edit-able settings override.</summary>
    public virtual bool IsDirty => false;

    /// <summary>Default MarkDirty/ClearDirty no-ops; subclasses with editable
    /// settings override to track the dirty state they care about.</summary>
    public virtual void MarkDirty() { }
    public virtual void ClearDirty() { }

    /// <summary>Default Save/Discard: no-op. Subclasses with persistent settings override.</summary>
    public virtual void Save() { }
    public virtual void Discard() { }

    public virtual void SetAsset(AssetInfo info)
    {
        Asset = info;
        AssetGuid = info.Guid;
        OnPropertyChanged(nameof(Title));
    }
}