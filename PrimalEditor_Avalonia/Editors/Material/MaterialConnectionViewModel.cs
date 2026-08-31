using System;
using System.Windows.Input;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using NodifyM.Avalonia.ViewModelBase;
using PrimalEditor_Avalonia.Native.MaterialGraph;

namespace PrimalEditor_Avalonia.Editors.Material;

/// <summary>
/// One connection between two pins. Carries the 1-based native ids for
/// round-tripping edits back to the engine via
/// <see cref="MaterialGraphSession.Connect"/> / <c>Disconnect</c>.
///
/// <para>
/// <see cref="DisconnectCommand"/> is bound by the ConnectionTemplate
/// to a small ✕ button overlaid on the line's midpoint. Clicking it
/// removes the connection both from the in-memory graph and the native
/// MaterialGraph, then re-serializes so the sidecar JSON round-trips
/// the deletion.
/// </para>
/// </summary>
public sealed class MaterialConnectionViewModel : ConnectionViewModelBase
{
    public uint FromNodeId { get; init; }
    public uint FromPin { get; init; }
    public uint ToNodeId { get; init; }
    public uint ToPin { get; init; }

    /// <summary>
    /// Editor VM that owns this connection. Captured at construction so
    /// the disconnect command can call back to
    /// <see cref="MaterialEditorViewModel.RemoveConnection"/> without
    /// having to find the active editor through a global locator. Stored
    /// separately from the base class's <c>editor</c> parameter because
    /// <c>MaterialEditorViewModel</c> does not inherit from
    /// <see cref="NodifyEditorViewModelBase"/> — the editor argument is
    /// a placeholder instance used only to satisfy the base ctor.
    /// </summary>
    private readonly MaterialEditorViewModel _owner;

    public MaterialConnectionViewModel(
        NodifyEditorViewModelBase editor,
        MaterialEditorViewModel owner,
        MaterialPinViewModel source,
        MaterialPinViewModel target)
        : base(editor, source, target)
    {
        _owner = owner;
        FromNodeId = source.NodeId;
        FromPin    = source.PinIndex;
        ToNodeId   = target.NodeId;
        ToPin      = target.PinIndex;
        DisconnectCommand = new RelayCommand(Disconnect);
    }

    /// <summary>
    /// Bound to the connection's ✕ button. Removes the connection from
    /// the live editor VM and the native MaterialGraph in one step so
    /// the right-hand preview reflects the new topology immediately.
    /// Silently no-ops when the editor VM has been disposed (the
    /// native graph is gone by then).
    /// </summary>
    public ICommand DisconnectCommand { get; }

    private void Disconnect()
    {
        try
        {
            var session = MaterialGraphSessionLocator.Current;
            if (session != null && session.IsValid)
            {
                session.Disconnect(FromNodeId, FromPin, ToNodeId, ToPin);
            }
            // Drop the in-memory VM entry. Captured at construction so we
            // don't have to walk a global locator just to find the editor.
            _owner?.RemoveConnection(this);
        }
        catch (System.Exception)
        {
            // Disconnect failures are usually stale connections (the
            // node moved or got deleted). Swallow so a half-dead
            // connection doesn't tear down the whole editor.
        }
    }
}