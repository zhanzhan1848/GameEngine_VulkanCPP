using System;
using System.Linq;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.VisualTree;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Material;

public partial class MaterialEditorView : UserControl
{
    /// <summary>
    /// Data format key shared between the palette drag source and the
    /// canvas drop target. The payload is a JSON-serialized
    /// <see cref="MaterialPaletteDragPayload"/> that the drop handler
    /// reads back to look up the <see cref="MaterialNodeTemplateViewModel"/>
    /// and call <see cref="MaterialEditorViewModel.AddNodeAt"/>.
    /// </summary>
    public const string PaletteDragFormat = "application/x-primal-material-palette";

    /// <summary>
    /// Cached references to <c>PaletteListBox</c> and <c>NodifyEditor</c>.
    /// Resolved by <see cref="FindControl{T}(string)"/> after
    /// <see cref="InitializeComponent"/> runs — the Avalonia source
    /// generator (11.3.17) is not generating these fields for this
    /// <c>Editors/Material/*.axaml</c> file in the current build graph,
    /// so the ctor would NRE on any direct field access (the original
    /// symptom: opening a material crashed the editor with NRE on
    /// <c>PaletteListBox.PointerMoved += ...</c>). Manual lookups keep
    /// the ctor safe even if the generator keeps skipping it.
    /// </summary>
    private Avalonia.Controls.ListBox? _paletteListBox;
    private NodifyM.Avalonia.Controls.NodifyEditor? _nodifyEditor;
    private Avalonia.Controls.Border? _paletteDragGhost;
    private Avalonia.Controls.TextBlock? _paletteDragGhostTitle;
    private Avalonia.Controls.TextBlock? _paletteDragGhostType;

    public MaterialEditorView()
    {
        InitializeComponent();

        _paletteListBox = this.FindControl<Avalonia.Controls.ListBox>("PaletteListBox");
        _nodifyEditor = this.FindControl<NodifyM.Avalonia.Controls.NodifyEditor>("NodifyEditor");
        _paletteDragGhost = this.FindControl<Avalonia.Controls.Border>("PaletteDragGhost");
        _paletteDragGhostTitle = this.FindControl<Avalonia.Controls.TextBlock>("PaletteDragGhostTitle");
        _paletteDragGhostType = this.FindControl<Avalonia.Controls.TextBlock>("PaletteDragGhostType");

        // Class-level DoubleTapped capture with handledEventsToo:true so we
        // swallow the gesture even when a child control (NodifyM Node) marks
        // it handled before bubbling reaches us. NodifyM 12.0.0 was crashing
        // the entire app on accidental double-tap of a node, likely a NRE in
        // its selection/layout path; this is the outermost safety net.
        AddHandler(DoubleTappedEvent, (EventHandler<TappedEventArgs>)OnAnyDoubleTapped,
            handledEventsToo: true);

        if (_paletteListBox != null)
        {
            // Palette drag-and-drop: track PointerMoved/Released on the ListBox
            // itself (not on the captured host). This matches the canonical
            // pattern in ContentBrowserView.axaml.cs which works reliably:
            // 1. PointerPressed on ListBoxItem records _dragCandidate + _dragStart
            // 2. PointerMoved on parent checks threshold and starts DoDragDropAsync
            // 3. PointerReleased clears state
            // Using the ListBox as the listener guarantees events bubble through
            // even when the cursor leaves the source row during the drag — no
            // pointer capture required, no async/await races.
            _paletteListBox.AddHandler(PointerPressedEvent,
                (EventHandler<PointerPressedEventArgs>)OnPalettePointerPressed,
                RoutingStrategies.Bubble,
                handledEventsToo: true);
        }

        // Keep palette dragging inside Avalonia's pointer pipeline. ListBoxItem
        // handles the press before a normal XAML handler can see it, while these
        // handledEventsToo handlers reliably observe the complete gesture.
        AddHandler(PointerMovedEvent, (EventHandler<PointerEventArgs>)OnPalettePointerMoved,
            RoutingStrategies.Bubble, handledEventsToo: true);
        AddHandler(PointerReleasedEvent, (EventHandler<PointerReleasedEventArgs>)OnPalettePointerReleased,
            RoutingStrategies.Bubble, handledEventsToo: true);

        if (_nodifyEditor != null)
        {
            DragDrop.SetAllowDrop(_nodifyEditor, true);
            _nodifyEditor.AddHandler(DragDrop.DragOverEvent,
                (EventHandler<DragEventArgs>)OnCanvasDragOver,
                RoutingStrategies.Bubble,
                handledEventsToo: true);
            _nodifyEditor.AddHandler(DragDrop.DropEvent,
                (EventHandler<DragEventArgs>)OnCanvasDrop,
                RoutingStrategies.Bubble,
                handledEventsToo: true);
        }

        AddHandler(KeyDownEvent, (EventHandler<KeyEventArgs>)OnEditorKeyDown,
            RoutingStrategies.Bubble,
            handledEventsToo: true);
        AddHandler(PointerPressedEvent, (EventHandler<PointerPressedEventArgs>)OnConnectionCutPointerPressed,
            RoutingStrategies.Bubble,
            handledEventsToo: true);
        AddHandler(PointerMovedEvent, (EventHandler<PointerEventArgs>)OnConnectionCutPointerMoved,
            RoutingStrategies.Bubble,
            handledEventsToo: true);

        // Catch-all on the editor surface itself: NodifyM 12.0.0 leaves
        // a stuck dashed PendingConnection line on the canvas if the
        // user starts a connection-drag and releases outside any valid
        // pin target. Clearing PendingConnectionProperty on every release
        // outside the canvas guarantees the dashed preview disappears,
        // even when DragOver/Drop never fires.
        AddHandler(PointerReleasedEvent, (EventHandler<PointerReleasedEventArgs>)OnSurfacePointerReleased,
            handledEventsToo: true);

        // Observe the connector's typed routed event as a safety net around
        // NodifyM's object-valued ConnectionCompletedCommand payload. This is
        // especially important for mixed input/output nodes such as
        // SampleTexture: the event already carries the exact connector VMs,
        // so there is no generic ValueTuple shape to lose at the command
        // boundary. The duplicate check keeps the normal command path cheap.
        AddHandler(NodifyM.Avalonia.Controls.Connector.PendingConnectionCompletedEvent,
            new NodifyM.Avalonia.Events.PendingConnectionEventHandler(OnPendingConnectionCompleted),
            RoutingStrategies.Bubble,
            handledEventsToo: true);

        // NodifyM 12.0.0's BaseNode.Location two-way binding back to the VM
        // does not fire reliably on every drag move — the visual position
        // updates, but MaterialNodeViewModel.Location stays stale, which
        // meant the sidecar JSON captured the wrong (x, y) when the user
        // moved a node. Hook the RoutedEvent at the UserControl level so
        // the event bubbles up from any BaseNode child, and forward the
        // new location into the VM ourselves. The visual binding still
        // works; we just belt-and-braces the round-trip so the sidecar
        // always sees the live position.
        AddHandler(NodifyM.Avalonia.Controls.BaseNode.LocationChangedEvent,
            new NodifyM.Avalonia.Events.NodeLocationEventHandler(OnAnyNodeLocationChanged),
            handledEventsToo: true);

        if (_paletteListBox == null)
        {
            Logger.Warning("MaterialEditorView: PaletteListBox not found; drag-and-drop is disabled for this editor open. The x:Name may be missing from the XAML or the source generator skipped this file.");
        }
    }

    private async void OnPickTexture(object? sender, RoutedEventArgs e)
    {
        if (sender is not Control { DataContext: MaterialParamViewModel parameter }) return;
        var topLevel = TopLevel.GetTopLevel(this);
        if (topLevel?.StorageProvider == null) return;

        var picker = new Avalonia.Platform.Storage.FilePickerOpenOptions
        {
            Title = "Choose imported engine texture",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new Avalonia.Platform.Storage.FilePickerFileType("Primal texture")
                {
                    Patterns = new[] { "*.texture" },
                },
            },
        };

        try
        {
            var contentRoot = AssetRegistry.ProjectContentRoot;
            if (!string.IsNullOrWhiteSpace(contentRoot))
            {
                picker.SuggestedStartLocation = await topLevel.StorageProvider
                    .TryGetFolderFromPathAsync(new Uri(contentRoot));
            }
        }
        catch { /* The picker still works without a suggested folder. */ }

        var files = await topLevel.StorageProvider.OpenFilePickerAsync(picker);
        if (files == null || files.Count == 0) return;
        var path = files[0].Path.LocalPath;
        if (!string.Equals(System.IO.Path.GetExtension(path), ".texture",
                StringComparison.OrdinalIgnoreCase)) return;

        var asset = AssetRegistry.Get(path) ?? AssetRegistry.Register(path);
        if (asset == null)
        {
            Logger.Warning($"Unable to register selected texture: {path}");
            return;
        }
        parameter.SelectTextureAsset(asset);
    }

    /// <summary>
    /// RoutedEvent sink for <see cref="NodifyM.Avalonia.Controls.BaseNode.LocationChangedEvent"/>.
    /// Fires whenever any node visual on the canvas moves — drag, snap,
    /// programmatic. Setting <c>node.Location = e.Location</c> triggers
    /// the VM's <see cref="System.ComponentModel.INotifyPropertyChanged.PropertyChanged"/>
    /// handler (wired in <see cref="MaterialNodeViewModel"/>'s ctor),
    /// which forwards to
    /// <see cref="MaterialEditorViewModel.OnNodeLocationChanged"/> to push
    /// the change into the native graph. So we only set the property
    /// here and let the existing VM→native pipeline do the rest.
    /// Without this hook, the VM's <c>Location</c> only updated on the
    /// initial bind and on <c>AddNode</c>'s cascade, leaving the sidecar
    /// JSON with the wrong position whenever the user dragged a node.
    /// </summary>
    private void OnAnyNodeLocationChanged(object? sender, NodifyM.Avalonia.Events.NodeLocationEventArgs e)
    {
        try
        {
            if (e?.Sender is not { } senderNode) return;
            if (senderNode.DataContext is not MaterialNodeViewModel node) return;
            // Suppress no-op writes to avoid dirtying the document for
            // every micro-move that didn't actually change the VM.
            if (node.Location == e.Location) return;
            node.Location = e.Location;
        }
        catch (System.Exception ex)
        {
            Logger.Error("MaterialEditorView.OnAnyNodeLocationChanged failed", ex);
        }
    }

    /// <summary>
    /// Clear any stuck <c>PendingConnection</c> preview line on the
    /// NodifyEditor when the user releases the pointer anywhere on the
    /// editor surface. NodifyM 12.0.0 has a bug where starting a
    /// connection drag from a pin and releasing outside any valid
    /// target leaves the dashed preview line stuck on the canvas. By
    /// listening at the UserControl level (with
    /// <c>handledEventsToo:true</c>) we catch the release even if a
    /// child control marks it handled.
    /// </summary>
    private void OnSurfacePointerReleased(object? sender, Avalonia.Input.PointerReleasedEventArgs e)
    {
        _lastRightCutPoint = null;
        ClearStuckPendingConnection();
    }

    private void OnPendingConnectionCompleted(
        object? sender,
        NodifyM.Avalonia.Events.PendingConnectionEventArgs e)
    {
        if (DataContext is not MaterialEditorViewModel vm
            || e.SourceConnector is not MaterialPinViewModel source
            || e.TargetConnector is not MaterialPinViewModel target)
            return;

        var from = source.IsInput ? target : source;
        var to = source.IsInput ? source : target;
        if (from.IsInput || !to.IsInput) return;

        var alreadyConnected = vm.Connections.Any(c =>
            c.FromNodeId == from.NodeId && c.FromPin == from.PinIndex &&
            c.ToNodeId == to.NodeId && c.ToPin == to.PinIndex);
        if (!alreadyConnected)
            vm.Connect(source, target);
    }

    private void InitializeComponent() => Avalonia.Markup.Xaml.AvaloniaXamlLoader.Load(this);

    // --- Palette → Canvas: double-click adds a node at the centre of the canvas ---

    private void OnPaletteDoubleTapped(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        if (DataContext is not MaterialEditorViewModel vm) return;
        if (TryFindTemplate(e.Source, out var template))
        {
            vm.AddNode(template);
            e.Handled = true;
        }
    }

    // --- Palette → Canvas: drag-and-drop with drop position ---

    /// <summary>
    /// Drag threshold (in DIPs) before a pressed-on-palette pointer press
    /// is treated as a drag. Matches the standard OS convention; without it
    /// every click would immediately start a drag and the palette list-box
    /// would never see a normal Tapped gesture for selection.
    /// </summary>
    private const double DragStartDistanceDip = 4.0;

    // Drag-and-drop state for the palette. Set by PointerPressed, cleared by
    // PointerReleased or after DoDragDropAsync completes. Same pattern as
    // ContentBrowserView.BeginPotentialAssetDrag/ClearPotentialAssetDrag.
    private MaterialNodeTemplateViewModel? _dragCandidate;
    private Avalonia.Point _dragStart;
    private bool _dragInProgress;

    private void OnPalettePointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        try
        {
            if (e.GetCurrentPoint(this).Properties.PointerUpdateKind
                != PointerUpdateKind.LeftButtonPressed) return;
            // Walk up from e.Source to the ListBoxItem. Handler is attached
            // to the ListBox so this works for clicks anywhere inside the row,
            // including ListBoxItem padding/margins that don't hit-test to
            // the StackPanel content. Without the walk-up the inner content
            // clicks would silently miss the handler.
            if (!TryFindTemplate(e.Source, out var template)) return;
            _dragCandidate = template;
            _dragStart = e.GetPosition(this);
        }
        catch (System.Exception ex)
        {
            Logger.Error("OnPalettePointerPressed failed", ex);
            ClearPotentialDrag();
        }
    }

    private void OnPalettePointerMoved(object? sender, Avalonia.Input.PointerEventArgs e)
    {
        if (_dragCandidate == null) return;

        var current = e.GetPosition(this);
        if (!_dragInProgress)
        {
            var dx = current.X - _dragStart.X;
            var dy = current.Y - _dragStart.Y;
            if (System.Math.Abs(dx) < DragStartDistanceDip &&
                System.Math.Abs(dy) < DragStartDistanceDip) return;

            _dragInProgress = true;
            if (_paletteDragGhostTitle != null)
                _paletteDragGhostTitle.Text = _dragCandidate.DisplayName;
            if (_paletteDragGhostType != null)
                _paletteDragGhostType.Text = _dragCandidate.TypeName;
            if (_paletteDragGhost != null)
                _paletteDragGhost.IsVisible = true;
        }

        if (_paletteDragGhost != null)
        {
            Avalonia.Controls.Canvas.SetLeft(_paletteDragGhost, current.X + 14);
            Avalonia.Controls.Canvas.SetTop(_paletteDragGhost, current.Y + 14);
        }
        e.Handled = true;
    }

    private void OnPalettePointerReleased(object? sender, Avalonia.Input.PointerReleasedEventArgs e)
    {
        try
        {
            if (_dragInProgress
                && _dragCandidate is { } template
                && _nodifyEditor != null
                && DataContext is MaterialEditorViewModel vm)
            {
                var editorPoint = e.GetPosition(_nodifyEditor);
                if (editorPoint.X >= 0 && editorPoint.Y >= 0
                    && editorPoint.X <= _nodifyEditor.Bounds.Width
                    && editorPoint.Y <= _nodifyEditor.Bounds.Height)
                {
                    vm.AddNodeAt(template, CanvasPointFromEditorPoint(editorPoint));
                    e.Handled = true;
                }
            }
        }
        catch (System.Exception ex)
        {
            Logger.Error("Material palette drop failed", ex);
        }
        finally
        {
            ClearPotentialDrag();
        }
    }

    private void ClearPotentialDrag()
    {
        if (_paletteDragGhost != null)
            _paletteDragGhost.IsVisible = false;
        _dragCandidate = null;
        _dragInProgress = false;
    }

    private void OnSaveClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is MaterialEditorViewModel vm)
        {
            vm.Save();
        }
    }

    private void OnDiscardClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is MaterialEditorViewModel vm)
        {
            vm.Discard();
        }
    }

    /// <summary>
    /// Update the editor's <see cref="MaterialEditorViewModel.SelectedNode"/>
    /// when the user clicks a node on the canvas. NodifyM 12 has no
    /// public <c>SelectionChanged</c> event we can hook from C#, so we
    /// rely on a <c>PointerPressed</c> capture on the Node DataTemplate
    /// root. <paramref name="e"/> arrives on the Node itself, whose
    /// DataContext is the <see cref="MaterialNodeViewModel"/> we want
    /// to surface in the right-hand panel.
    /// </summary>
    private void OnNodePointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        try
        {
            if (sender is Avalonia.Controls.Control c
                && c.DataContext is MaterialNodeViewModel node
                && DataContext is MaterialEditorViewModel vm)
            {
                vm.SelectedNode = node;
                _nodifyEditor?.Focus();
            }
        }
        catch (System.Exception ex)
        {
            Logger.Error("MaterialEditorView.OnNodePointerPressed failed", ex);
        }
    }

    private void OnEditorKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key is not (Key.Delete or Key.Back)) return;
        if (e.Source is TextBox) return;
        if (DataContext is MaterialEditorViewModel vm && vm.RemoveSelectedNode())
        {
            e.Handled = true;
        }
    }

    private void OnConnectionCutPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        // This outer handledEventsToo route is the most reliable place to
        // begin a palette gesture: ListBoxItem consumes PointerPressed before
        // ordinary handlers, especially on macOS.
        if (e.GetCurrentPoint(this).Properties.PointerUpdateKind
                == PointerUpdateKind.LeftButtonPressed
            && TryFindTemplate(e.Source, out var paletteTemplate))
        {
            _dragCandidate = paletteTemplate;
            _dragStart = e.GetPosition(this);
        }

        if (e.Source is Control control)
        {
            var nodeVisual = control as NodifyM.Avalonia.Controls.BaseNode
                ?? control.FindAncestorOfType<NodifyM.Avalonia.Controls.BaseNode>();
            if (nodeVisual?.DataContext is MaterialNodeViewModel node
                && DataContext is MaterialEditorViewModel vm)
            {
                vm.SelectedNode = node;
                if (control is not TextBox) _nodifyEditor?.Focus();
            }
        }

        if (e.GetCurrentPoint(this).Properties.PointerUpdateKind != PointerUpdateKind.RightButtonPressed)
            return;
        if (_nodifyEditor == null) return;

        var point = CanvasPointFromEditorPoint(e.GetPosition(_nodifyEditor));
        _lastRightCutPoint = point;
        if (TryRemoveConnectionAtEventSource(e.Source)
            || TryRemoveConnectionAlongStroke(point, point))
            e.Handled = true;
    }

    private void OnConnectionCutPointerMoved(object? sender, PointerEventArgs e)
    {
        if (!e.GetCurrentPoint(this).Properties.IsRightButtonPressed) return;
        if (_nodifyEditor == null) return;

        var point = CanvasPointFromEditorPoint(e.GetPosition(_nodifyEditor));
        var previous = _lastRightCutPoint ?? point;
        _lastRightCutPoint = point;
        if (TryRemoveConnectionAtEventSource(e.Source)
            || TryRemoveConnectionAlongStroke(previous, point))
            e.Handled = true;
    }

    private Avalonia.Point? _lastRightCutPoint;

    /// <summary>
    /// Disconnect the first connection intersected by the right-button stroke.
    /// Nodify captures right-button input on the editor, so this must use graph
    /// geometry instead of relying on the connection being the event source.
    /// </summary>
    private bool TryRemoveConnectionAlongStroke(Avalonia.Point strokeStart, Avalonia.Point strokeEnd)
    {
        if (DataContext is not MaterialEditorViewModel vm) return false;

        var zoom = _nodifyEditor?.Zoom ?? 1d;
        var tolerance = 9d / System.Math.Max(zoom, 0.1d);
        foreach (var connection in vm.Connections.ToList())
        {
            if (ConnectionIntersectsStroke(
                    connection.Source.Anchor,
                    connection.Target.Anchor,
                    strokeStart,
                    strokeEnd,
                    tolerance))
            {
                vm.RemoveConnection(connection);
                return true;
            }
        }
        return false;
    }

    internal static bool ConnectionIntersectsStroke(
        Avalonia.Point source,
        Avalonia.Point target,
        Avalonia.Point strokeStart,
        Avalonia.Point strokeEnd,
        double tolerance)
    {
        // Match NodifyM.Connection.DrawLineGeometry: short straight sections
        // at each pin, then its cubic Bezier control-point calculation.
        const double spacing = 20d;
        var curveStart = new Avalonia.Point(source.X + spacing, source.Y);
        var curveEnd = new Avalonia.Point(target.X - spacing, target.Y);
        if (SegmentsNear(source, curveStart, strokeStart, strokeEnd, tolerance)
            || SegmentsNear(curveEnd, target, strokeStart, strokeEnd, tolerance))
            return true;

        var dx = System.Math.Abs(target.X - source.X);
        var dy = System.Math.Abs(target.Y - source.Y);
        var offset = System.Math.Max(System.Math.Min(100d, dy), dx / 2d);
        offset = System.Math.Min(100d + System.Math.Sqrt(dx * 25d), offset);
        var control1 = new Avalonia.Point(curveStart.X + offset, curveStart.Y);
        var control2 = new Avalonia.Point(curveEnd.X - offset, curveEnd.Y);

        var previous = curveStart;
        const int samples = 32;
        for (var i = 1; i <= samples; i++)
        {
            var t = i / (double)samples;
            var u = 1d - t;
            var current = new Avalonia.Point(
                u * u * u * curveStart.X + 3d * u * u * t * control1.X
                    + 3d * u * t * t * control2.X + t * t * t * curveEnd.X,
                u * u * u * curveStart.Y + 3d * u * u * t * control1.Y
                    + 3d * u * t * t * control2.Y + t * t * t * curveEnd.Y);
            if (SegmentsNear(previous, current, strokeStart, strokeEnd, tolerance)) return true;
            previous = current;
        }
        return false;
    }

    private static bool SegmentsNear(
        Avalonia.Point a, Avalonia.Point b,
        Avalonia.Point c, Avalonia.Point d,
        double tolerance)
    {
        if (SegmentsIntersect(a, b, c, d)) return true;
        var limit = tolerance * tolerance;
        return PointSegmentDistanceSquared(a, c, d) <= limit
            || PointSegmentDistanceSquared(b, c, d) <= limit
            || PointSegmentDistanceSquared(c, a, b) <= limit
            || PointSegmentDistanceSquared(d, a, b) <= limit;
    }

    private static bool SegmentsIntersect(
        Avalonia.Point a, Avalonia.Point b,
        Avalonia.Point c, Avalonia.Point d)
    {
        static double Cross(Avalonia.Point p, Avalonia.Point q, Avalonia.Point r) =>
            (q.X - p.X) * (r.Y - p.Y) - (q.Y - p.Y) * (r.X - p.X);
        var abC = Cross(a, b, c);
        var abD = Cross(a, b, d);
        var cdA = Cross(c, d, a);
        var cdB = Cross(c, d, b);
        return abC * abD <= 0d && cdA * cdB <= 0d;
    }

    private static double PointSegmentDistanceSquared(
        Avalonia.Point p, Avalonia.Point a, Avalonia.Point b)
    {
        var dx = b.X - a.X;
        var dy = b.Y - a.Y;
        var lengthSquared = dx * dx + dy * dy;
        if (lengthSquared <= double.Epsilon)
        {
            var px = p.X - a.X;
            var py = p.Y - a.Y;
            return px * px + py * py;
        }

        var t = System.Math.Clamp(
            ((p.X - a.X) * dx + (p.Y - a.Y) * dy) / lengthSquared,
            0d,
            1d);
        var ex = p.X - (a.X + t * dx);
        var ey = p.Y - (a.Y + t * dy);
        return ex * ex + ey * ey;
    }

    private bool TryRemoveConnectionAtEventSource(object? source)
    {
        if (DataContext is not MaterialEditorViewModel vm || source is not Control control)
            return false;

        var visual = control as NodifyM.Avalonia.Controls.BaseConnection
            ?? control.FindAncestorOfType<NodifyM.Avalonia.Controls.BaseConnection>();
        if (visual?.DataContext is not MaterialConnectionViewModel connection) return false;

        vm.RemoveConnection(connection);
        return true;
    }

    /// <summary>
    /// Swallow <see cref="InputElement.PointerPressed"/> on any input
    /// control inside a node body so NodifyM's
    /// <c>BaseNode.OnPointerPressed</c> sees <c>e.Handled == true</c> and
    /// skips <c>StartNodeDrag</c>. NodifyM 12.0.0 only bails for
    /// <c>ComboBox</c> parents; without this handler clicking a TextBox
    /// captures the pointer for a node-drag instead of giving the
    /// TextBox keyboard focus, so users can't type into the inline
    /// parameter boxes. Set on every TextBox in
    /// <see cref="MaterialEditorView.axaml"/> (the inline node body and
    /// the right-hand inspector panel).
    /// </summary>
    private void OnNodeInputPointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        // Mark the routed event as handled. NodifyM's drag-start check is
        // `if (_editor != null && !e.Handled) StartNodeDrag(...)`, so any
        // true here prevents the node from being dragged when the user
        // just wanted to type. Don't suppress focus acquisition — the
        // TextBox still gets focus through the standard input pipeline.
        e.Handled = true;
    }

    /// <summary>
    /// Commit a TextBox value on Enter. Avalonia's TwoWay bindings push
    /// every keystroke back to the VM, which already calls
    /// <c>PushToNative</c> + <c>RefreshPreview</c>, so per-character
    /// refresh is technically enough — but the user expects pressing
    /// Enter to be the explicit "I'm done" gesture that flushes any
    /// partially-typed value and visually refreshes the preview. We
    /// force a preview push (independent of whether the binding saw a
    /// change), drop keyboard focus so the inline number isn't
    /// visually selected, and mark the event handled so it doesn't
    /// bubble to NodifyM.
    /// </summary>
    private void OnNodeInputKeyDown(object? sender, Avalonia.Input.KeyEventArgs e)
    {
        if (e.Key != Avalonia.Input.Key.Enter) return;
        // Re-serialize + push the preview so any pending value lands.
        MaterialGraphSessionLocator.RefreshPreview();
        // Move focus off the TextBox so subsequent Space/Enter keystrokes
        // don't re-trigger the textbox commit on the same row. We can't
        // call tb.ClearFocus() directly (Avalonia 12 dropped that helper);
        // instead move focus to the parent canvas — same end result.
        if (sender is Avalonia.Controls.TextBox tb && tb.Parent is Avalonia.Input.InputElement parent)
        {
            parent.Focus();
        }
        e.Handled = true;
    }

    private void OnCanvasDragOver(object? sender, Avalonia.Input.DragEventArgs e)
    {
        try
        {
            // Always also clear any leftover PendingConnection from a
            // previous connection-drag attempt. Without this, NodifyM
            // 12.0.0 leaves a stuck dashed line on the canvas.
            ClearStuckPendingConnection();

            if (e.DataTransfer.Contains(Avalonia.Input.DataFormat.CreateStringApplicationFormat(PaletteDragFormat)) ||
                TryGetDroppedTexture(e) != null)
                e.DragEffects = Avalonia.Input.DragDropEffects.Copy;
            else
                e.DragEffects = Avalonia.Input.DragDropEffects.None;
        }
        catch (System.Exception ex)
        {
            Logger.Error("MaterialEditorView.OnCanvasDragOver failed", ex);
            e.DragEffects = Avalonia.Input.DragDropEffects.None;
        }
    }

    private void OnCanvasDrop(object? sender, Avalonia.Input.DragEventArgs e)
    {
        try
        {
            if (DataContext is not MaterialEditorViewModel vm) return;
            if (_nodifyEditor == null) return;
            var format = Avalonia.Input.DataFormat.CreateStringApplicationFormat(PaletteDragFormat);
            if (!e.DataTransfer.Contains(format))
            {
                if (TryGetDroppedTexture(e) is { } texture)
                {
                    var targetVisual = e.Source as Control;
                    var targetNode = (targetVisual as NodifyM.Avalonia.Controls.BaseNode
                        ?? targetVisual?.FindAncestorOfType<NodifyM.Avalonia.Controls.BaseNode>())
                        ?.DataContext as MaterialNodeViewModel;
                    var assigned = targetNode != null && vm.AssignTextureAsset(targetNode, texture);
                    if (!assigned)
                    {
                        vm.AddTextureAssetAt(texture,
                            CanvasPointFromEditorPoint(e.GetPosition(_nodifyEditor)));
                    }
                    e.DragEffects = Avalonia.Input.DragDropEffects.Copy;
                    e.Handled = true;
                    return;
                }
                // Not a palette drop — but if NodifyM left a PendingConnection
                // visualization stuck (failed connection drag from a pin),
                // the user releasing on the canvas is the right moment to
                // clear it. PendingConnection is a StyledProperty<object> on
                // NodifyEditor and setting it to null hides the dashed line.
                ClearStuckPendingConnection();
                return;
            }
            var text = e.DataTransfer.TryGetValue(format);
            if (string.IsNullOrEmpty(text)) return;
            var payload = MaterialPaletteDragPayload.Deserialize(text);
            if (payload == null) return;
            if (vm.NodePalette.FirstOrDefault(t => t.TypeName == payload.TypeName) is not { } template)
                return;

            // Position the node at the drop location relative to the canvas.
            // Map the visual drop point through the editor's pan/zoom so the
            // node lands in canvas-space coordinates (not editor-local coords
            // after the translate+scale applied by NodifyM).
            var position = CanvasPointFromEditorPoint(e.GetPosition(_nodifyEditor));
            vm.AddNodeAt(template, position);
            e.DragEffects = Avalonia.Input.DragDropEffects.Copy;
            e.Handled = true;
        }
        catch (System.Exception ex)
        {
            Logger.Error("MaterialEditorView.OnCanvasDrop failed", ex);
            e.DragEffects = Avalonia.Input.DragDropEffects.None;
        }
    }

    private static AssetInfo? TryGetDroppedTexture(Avalonia.Input.DragEventArgs e)
    {
        var payload = AssetDropHandler.ReadPayload(e);
        var asset = payload == null ? null : AssetDropHandler.Resolve(payload);
        if (asset == null)
        {
            var path = AssetDropHandler.FirstFilePath(e);
            if (!string.IsNullOrWhiteSpace(path))
                asset = AssetRegistry.Get(path) ?? AssetRegistry.Register(path);
        }
        return asset is { Type: AssetType.Texture } &&
               asset.Extension.Equals(".texture", StringComparison.OrdinalIgnoreCase)
            ? asset
            : null;
    }

    /// <summary>
    /// Force-clear any leftover <c>PendingConnection</c> visualization.
    /// NodifyM 12.0.0 has a bug where starting a connection drag from a
    /// pin and releasing outside any valid target leaves the dashed
    /// preview line stuck on the canvas. The <c>PendingConnection</c>
    /// StyledProperty controls the visual; clearing it via reflection
    /// is the only reliable workaround without patching the library.
    /// </summary>
    private void ClearStuckPendingConnection()
    {
        try
        {
            // PendingConnection is a StyledProperty<object> on NodifyEditor.
            // Setting it to null hides the dashed line preview. The
            // property is static on the type, not on the instance.
            var prop = NodifyM.Avalonia.Controls.NodifyEditor.PendingConnectionProperty;
            if (prop == null) return;
            if (_nodifyEditor == null) return;
            _nodifyEditor.SetCurrentValue(prop, null);
        }
        catch (System.Exception ex)
        {
            Logger.Error("ClearStuckPendingConnection failed", ex);
        }
    }

    /// <summary>
    /// Convert a point in <see cref="NodifyEditor"/>'s local (post-transform)
    /// coordinate space into canvas-space coordinates that match the
    /// <see cref="NodifyM.Avalonia.ViewModelBase.NodeViewModel.Location"/>
    /// values bound to the editor. When pan = (0,0) and zoom = 1 this is a
    /// no-op; otherwise the editor's
    /// <see cref="NodifyEditor.ViewTranslateTransform"/> shifts its content
    /// by <c>(-OffsetX, -OffsetY)</c> and <see cref="NodifyEditor.Zoom"/>
    /// scales around <see cref="NodifyEditor.ZoomCenter"/>, so we must apply
    /// the inverse to drop the new node where the user actually released.
    /// </summary>
    private Avalonia.Point CanvasPointFromEditorPoint(Avalonia.Point editor)
    {
        if (_nodifyEditor == null) return editor;
        // NodifyM applies OffsetX/Y as an internal translation. The editor's
        // zoom is a RenderTransform on the editor itself, so GetPosition has
        // already returned pre-transform coordinates and must not be divided
        // by Zoom again.
        return new Avalonia.Point(
            editor.X - _nodifyEditor.OffsetX,
            editor.Y - _nodifyEditor.OffsetY);
    }

    /// <summary>
    /// Hard-disables double-click on a material node. Two layers of
    /// defense, because NodifyM 12.0.0 was crashing the entire app when
    /// the user accidentally double-tapped a node — likely a
    /// <see cref="NullReferenceException"/> in the control's selection
    /// or layout logic that bubbled out as an unhandled exception:
    ///
    /// <list type="number">
    ///   <item>The XAML wires this handler as the first subscriber on the
    ///         Node template's <c>DoubleTapped</c> event.</item>
    ///   <item>The try-catch guarantees no exception escapes the editor
    ///         surface even if NodifyM throws synchronously from the
    ///         same event path.</item>
    ///   <item>The ctor installs a class-level handler with
    ///         <c>handledEventsToo:true</c> so we still swallow the
    ///         gesture even if a child control marks it Handled before
    ///         bubbling reaches us.</item>
    /// </list>
    /// Single-click selection still works — only the double-click
    /// gesture is suppressed.
    /// </summary>
    private void OnNodeDoubleTapped(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        try
        {
            e.Handled = true;
        }
        catch (System.Exception ex)
        {
            Logger.Error("Material node double-tap handler failed", ex);
            e.Handled = true;
        }
    }

    /// <summary>
    /// Outermost safety net for the same NodifyM 12.0.0 crash: a
    /// class-level <c>DoubleTapped</c> capture on the editor surface
    /// itself, registered with <c>handledEventsToo:true</c> so it fires
    /// even after NodifyM's internal handlers have marked the event
    /// Handled. Anything bubbling out of a child Node goes through here,
    /// where we swallow the event and log any unexpected exception so
    /// it can't reach the dispatcher and tear down the app.
    /// </summary>
    private void OnAnyDoubleTapped(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        try
        {
            e.Handled = true;
        }
        catch (System.Exception ex)
        {
            Logger.Error("MaterialEditorView DoubleTapped capture failed", ex);
            e.Handled = true;
        }
    }

    /// <summary>
    /// Walks up the visual tree from <paramref name="source"/> until it
    /// finds a <see cref="ListBoxItem"/> with a
    /// <see cref="MaterialNodeTemplateViewModel"/> as its DataContext.
    /// More robust than reading <c>ListBox.SelectedItem</c>, which can
    /// be null when <c>DoubleTapped</c> fires before selection has
    /// settled in Avalonia's event pipeline.
    /// </summary>
    private static bool TryFindTemplate(object? source, out MaterialNodeTemplateViewModel template)
    {
        template = null!;
        if (source is Avalonia.Controls.Control c)
        {
            var item = c as Avalonia.Controls.ListBoxItem
                ?? c.FindAncestorOfType<Avalonia.Controls.ListBoxItem>();
            if (item?.DataContext is MaterialNodeTemplateViewModel t)
            {
                template = t;
                return true;
            }
        }
        return false;
    }
}

/// <summary>
/// JSON-encoded payload carried by the palette drag. We only need the
/// type name (display name is purely cosmetic and not used by the
/// drop handler). Kept in a tiny POCO so it can evolve without
/// breaking in-flight drags.
/// </summary>
public sealed class MaterialPaletteDragPayload
{
    public string TypeName { get; set; } = string.Empty;
    public string DisplayName { get; set; } = string.Empty;

    public MaterialPaletteDragPayload() { }
    public MaterialPaletteDragPayload(string typeName, string displayName)
    {
        TypeName = typeName;
        DisplayName = displayName;
    }

    public string Serialize() => System.Text.Json.JsonSerializer.Serialize(this);

    public static MaterialPaletteDragPayload? Deserialize(string json)
    {
        try { return System.Text.Json.JsonSerializer.Deserialize<MaterialPaletteDragPayload>(json); }
        catch { return null; }
    }
}
