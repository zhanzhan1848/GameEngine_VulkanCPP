using System;
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
    void SetAsset(AssetInfo info);
}

public abstract class AssetEditorViewModel : ViewModelBase, IAssetEditor
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

    public virtual void SetAsset(AssetInfo info)
    {
        Asset = info;
        AssetGuid = info.Guid;
        OnPropertyChanged(nameof(Title));
    }
}