using System;
using System.Collections.ObjectModel;
using Avalonia;
using NodifyM.Avalonia.ViewModelBase;
using PrimalEditor_Avalonia.ViewModels.PFX;

namespace PrimalEditor_Avalonia.ViewModels.PFX;

/// <summary>
/// Canvas visuals for the PFX module stack: the emitter's execution pipeline
/// drawn as a node chain (Emitter → module instances in stack order → Output).
/// The graph is structural — the engine executes the stack linearly, so the
/// edges are fixed and not user-wirable; clicking a node selects its module
/// for the parameter panel.
/// </summary>
public sealed class PfxNodeViewModel : NodeViewModelBase
{
    public string ModuleName { get; init; } = "";
    public string Subtitle { get; init; } = "";
    public bool IsTerminal { get; init; }

    /// <summary>The stack row this node visualizes (null = terminal node).</summary>
    public PfxStackModuleRow? Module { get; init; }

    public new ObservableCollection<PfxPinViewModel> Input { get; } = new();
    public new ObservableCollection<PfxPinViewModel> Output { get; } = new();

    public new string Title
    {
        get => base.Title ?? ModuleName;
        set => base.Title = value;
    }
}

public sealed class PfxPinViewModel : ConnectorViewModelBase
{
}

/// <summary>Fixed stage-flow edge between consecutive pipeline nodes.</summary>
public sealed class PfxConnectionViewModel : ConnectionViewModelBase
{
    public PfxConnectionViewModel(PfxPinViewModel source, PfxPinViewModel target)
        : base(PfxEditorPlaceholder.Instance, source, target)
    {
    }
}

/// <summary>
/// ConnectionViewModelBase requires an editor VM; the PFX graph has no
/// user-created connections to route through it, so a shared placeholder
/// satisfies the base constructor (same trick as the material editor).
/// </summary>
public sealed class PfxEditorPlaceholder : NodifyEditorViewModelBase
{
    public static PfxEditorPlaceholder Instance { get; } = new();
    private PfxEditorPlaceholder() { }
}
