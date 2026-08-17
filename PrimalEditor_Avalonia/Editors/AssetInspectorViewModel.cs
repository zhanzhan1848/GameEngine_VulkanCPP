using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Editors;

/// <summary>
/// Base ViewModel for an asset inspector/editable preview. Subclasses
/// provide the type-specific preview surface and inspector sections;
/// the base wires into the shared AssetEditor framework (Undo/Redo,
/// Dirty tracking, Save/Discard, layout persistence) and exposes the
/// extension hooks future inspectors (Texture, Shader, ...) override.
/// </summary>
public abstract class AssetInspectorViewModel : AssetEditorViewModel
{
    private string _statusMessage = string.Empty;
    private AssetNode? _selectedNode;

    protected AssetInspectorViewModel(AssetInfo asset) : base()
    {
        SetAsset(asset);
    }

    public string StatusMessage
    {
        get => _statusMessage;
        protected set
        {
            _statusMessage = value;
            OnPropertyChanged();
        }
    }

    public AssetNode? SelectedNode
    {
        get => _selectedNode;
        set
        {
            if (_selectedNode == value) return;
            _selectedNode = value;
            OnPropertyChanged();
            OnNodeSelected(value);
        }
    }

    public abstract IReadOnlyList<AssetNode> TreeRoots { get; }
    public abstract IReadOnlyList<InspectorSection> InspectorSections { get; }
    public virtual IReadOnlyList<IPreviewAction> PreviewActions => Array.Empty<IPreviewAction>();
    public virtual bool IsDraggable(AssetNode node) => true;

    protected virtual void OnNodeSelected(AssetNode? node) { }
}

public sealed record InspectorSection(string Title, IReadOnlyList<InspectorRow> Rows);

public sealed record InspectorRow(string Label, string Value);

public interface IPreviewAction
{
    string Label { get; }
    void Execute(AssetInspectorViewModel inspector);
}