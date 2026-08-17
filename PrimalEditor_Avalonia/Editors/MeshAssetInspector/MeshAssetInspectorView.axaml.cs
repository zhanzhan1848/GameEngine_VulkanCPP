using System;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Editors.MeshAssetInspector;

public partial class MeshAssetInspectorView : UserControl
{
    private const double DragThreshold = 5.0;
    private AssetNode? _dragCandidate;
    private Avalonia.Point _dragStartPoint;

    public MeshAssetInspectorView()
    {
        InitializeComponent();
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    protected override void OnAttachedToVisualTree(Avalonia.VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        // Wire up tree-node pointer events so the user can drag a node
        // out to World Editor — same shape as ContentBrowserView but
        // scoped to this inspector's TreeView. Tunnel routing lets us
        // catch the press before its inner item eats it.
        if (this.FindControl<TreeView>("InspectorTree") is { } tree)
        {
            tree.AddHandler(PointerPressedEvent, OnTreePointerPressed, Avalonia.Interactivity.RoutingStrategies.Tunnel);
            tree.AddHandler(PointerMovedEvent, OnTreePointerMoved, Avalonia.Interactivity.RoutingStrategies.Tunnel);
        }
    }

    private void OnTreePointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        var point = e.GetCurrentPoint(this);
        if (!point.Properties.IsLeftButtonPressed) return;
        var node = TryResolveNode(e);
        if (node == null) return;
        if (DataContext is not AssetInspectorViewModel vm) return;
        if (!vm.IsDraggable(node)) return;
        _dragCandidate = node;
        _dragStartPoint = point.Position;
    }

    private void OnTreePointerMoved(object? sender, Avalonia.Input.PointerEventArgs e)
    {
        if (_dragCandidate == null) return;
        var point = e.GetCurrentPoint(this);
        var dx = point.Position.X - _dragStartPoint.X;
        var dy = point.Position.Y - _dragStartPoint.Y;
        if (Math.Sqrt(dx * dx + dy * dy) < DragThreshold) return;
        if (DataContext is not AssetInspectorViewModel vm) return;
        var node = _dragCandidate;
        _dragCandidate = null;
        BeginDrag(vm, node, e);
    }

    private void BeginDrag(AssetInspectorViewModel vm, AssetNode node, Avalonia.Input.PointerEventArgs e)
    {
        var asset = vm.Asset;
        if (asset == null) return;
        var payload = new AssetDragPayload(
            Guid: asset.Guid,
            FullPath: asset.FullPath,
            IsContainer: node.IsContainer,
            ParentGuid: asset.Guid);
        var format = Avalonia.Input.DataFormat.CreateStringApplicationFormat(AssetDragPayload.DataFormat);
        var transfer = new Avalonia.Input.DataTransfer();
        transfer.Add(Avalonia.Input.DataTransferItem.Create(format, payload.Serialize()));
        _ = Avalonia.Input.DragDrop.DoDragDropAsync(e, transfer, Avalonia.Input.DragDropEffects.Copy);
    }

    private static AssetNode? TryResolveNode(Avalonia.Input.PointerEventArgs e)
    {
        var current = e.Source as Avalonia.Controls.Control;
        while (current != null)
        {
            if (current.DataContext is AssetNode node) return node;
            current = current.Parent as Avalonia.Controls.Control;
        }
        return null;
    }
}