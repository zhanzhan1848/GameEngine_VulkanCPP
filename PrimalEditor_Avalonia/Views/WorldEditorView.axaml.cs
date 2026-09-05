using System;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Entities;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;
using PrimalEditor_Avalonia.Views.Engine3D;

namespace PrimalEditor_Avalonia.Views;

    public partial class WorldEditorView : UserControl
    {
        public static readonly StyledProperty<bool> IsBuildingProperty =
            AvaloniaProperty.Register<WorldEditorView, bool>(nameof(IsBuilding));

        public bool IsBuilding
        {
            get => GetValue(IsBuildingProperty);
            private set => SetValue(IsBuildingProperty, value);
        }

        public WorldEditorView()
        {
            InitializeComponent();
            DataContextChanged += OnDataContextChanged;
            KeyDown += OnWorldEditorKeyDown;
            // Polling timer that mirrors BuildService.IsRunning into the
            // view's IsBuilding property. The static BuildService class
            // has no events for the running→stopped transition, so a
            // 100ms UI-thread poll is the cheapest reliable signal.
            _buildPollTimer = new Avalonia.Threading.DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(100),
            };
            _buildPollTimer.Tick += (_, _) => IsBuilding = BuildService.IsRunning;
            _buildPollTimer.Start();
        }

private EngineViewport3D? _viewport;
    private WorldEditorViewModel? _vm;
    private Avalonia.Threading.DispatcherTimer? _buildPollTimer;

    private static MainWindowViewModel? ResolveMainVm()
    {
        if (Avalonia.Application.Current?.ApplicationLifetime
            is Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop)
        {
            return desktop.MainWindow?.DataContext as MainWindowViewModel;
        }
        return null;
    }

    private void OnDataContextChanged(object? sender, System.EventArgs e)
    {
        if (_viewport != null)
        {
            _viewport.EntityPicked -= OnEntityPicked;
            _viewport.GizmoDrag -= OnGizmoDrag;
        }
        if (_vm != null)
        {
            _vm.PickerCandidatesChanged -= OnPickerCandidatesChanged;
            // Move the gizmo when the user picks a different entity
            // (via the picker OR the hierarchy tree). Without this the
            // gizmo stays on the previously-selected entity, and
            // dragging it pushes the wrong one.
            _vm.PropertyChanged -= OnViewModelPropertyChanged;
        }
        _vm = DataContext as WorldEditorViewModel;
        _viewport = this.FindControl<EngineViewport3D>("WorldViewport");
        var mainVm = ResolveMainVm();
        if (mainVm != null && this.FindControl<LoggerView>("LoggerTabView") is { } loggerView)
        {
            loggerView.DataContext = mainVm.Logger;
        }
        if (mainVm != null && this.FindControl<ContentBrowserView>("ContentBrowserBottomView") is { } cbView)
        {
            cbView.DataContext = mainVm.ContentBrowser;
        }
        if (_vm != null)
        {
            _vm.PickerCandidatesChanged += OnPickerCandidatesChanged;
            _vm.PropertyChanged += OnViewModelPropertyChanged;

            // Wire the look-through-scene-camera callbacks so the
            // Properties panel's "Look Through This Camera" button can
            // route to the viewport without taking a hard dependency
            // on EngineViewport3D from the VM (the VM is shared across
            // many views; only this view has a viewport).
            _vm.Properties.LookThroughCameraCallback = id =>
            {
                _viewport?.LookThroughCamera(id);
                _vm.Properties.NotifyLookThroughChanged();
            };
            _vm.Properties.IsLookingThroughCameraQuery =
                id => _viewport != null && _viewport.IsLookingThroughSceneCamera
                      && _viewport.LookThroughSceneEntityId == id;
            _vm.Properties.SetMainCameraCallback = () =>
            {
                if (_vm.ActiveScene == null || _vm.SelectedEntities.Length != 1) return;
                _vm.ActiveScene.Environment.MainCameraEntityGuid = _vm.SelectedEntities[0].EntityGuid.Value;
                _vm.ActiveScene.MarkDirty();
                _vm.MarkProjectDirty();
                _vm.Properties.NotifyMainCameraChanged();
            };
            _vm.Properties.IsMainCameraQuery = () =>
                _vm.ActiveScene?.Environment.MainCameraEntityGuid ==
                (_vm.SelectedEntities.Length == 1 ? _vm.SelectedEntities[0].EntityGuid.Value : null);
        }
        if (_viewport != null)
        {
            _viewport.EntityPicked += OnEntityPicked;
            _viewport.GizmoDrag += OnGizmoDrag;
            _viewport.SetScene(_vm?.ActiveScene);
            // Selection outline (plan §6.1 A1): per-frame AABB query for
            // the selection only.
            _viewport.SelectionBoundsProvider = ComputeSelectionBounds;
            // GI volume wireframe (plan §6.2 B1 L1).
            _viewport.VolumeBoxProvider = ComputeVolumeBoxes;
            // DDGI probe lattice dots (plan §6.2 B1 L1.5).
            _viewport.ProbePointsProvider = ComputeProbePoints;
            // A7 视图模式: all-entity bounds overlay; returns empty while
            // the toolbar "Bounds" toggle is off (no per-frame AABB storm).
            _viewport.AllBoundsProvider = () => _vm.GetAllEntityBounds();
            // Gizmo undo + snap accumulators (plan §6.1 A2).
            _viewport.GizmoDragStart += OnGizmoDragStart;
            _viewport.GizmoDragEnd += OnGizmoDragEnd;
            // Track Ctrl for "hold to bypass snap" during gizmo drags.
            _viewport.PointerPressed += (_, e) =>
                _ctrlDown = e.KeyModifiers.HasFlag(Avalonia.Input.KeyModifiers.Control);
            WireHierarchyDragDrop();
        }
        RefreshPicker();
    }

    private bool _ctrlDown;

    private System.Collections.Generic.IReadOnlyList<ViewportPicker.AABB> ComputeVolumeBoxes()
    {
        var box = _vm?.SceneEnvironmentViewModel.GiVolumes.SelectedBox;
        return box != null
            ? new[] { box.Value }
            : System.Array.Empty<ViewportPicker.AABB>();
    }

    private System.Collections.Generic.IReadOnlyList<(float, float, float)> ComputeProbePoints()
        => _vm?.SceneEnvironmentViewModel.GiVolumes.ComputeProbePoints()
           ?? System.Array.Empty<(float, float, float)>();

    private System.Collections.Generic.IReadOnlyList<ViewportPicker.AABB> ComputeSelectionBounds()
    {
        if (_vm == null || _vm.SelectedEntities.Length == 0)
            return System.Array.Empty<ViewportPicker.AABB>();
        var list = new System.Collections.Generic.List<ViewportPicker.AABB>();
        foreach (var entity in _vm.SelectedEntities)
        {
            var id = _vm.GetNativeEntityId(entity);
            if (id == 0) continue;
            if (NativeEngineBridge.TryGetEntityWorldAABB(id, out var b))
                list.Add(new ViewportPicker.AABB(b.minX, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ));
        }
        return list;
    }

    // =======================================================================
    // Gizmo drag: multi-select + snap + undo (plan §6.1 A2)
    // =======================================================================

    // Captured at GizmoDragStart; committed as one MultiEntityTransformAction
    // at GizmoDragEnd if any delta was actually applied.
    private System.Collections.Generic.List<(Components.GameEntity entity, Common.TransformData before)>? _dragBefore;
    private bool _dragApplied;

    // Accumulators for the current (mode, axis) gesture. Snapping applies
    // to the accumulated delta so tiny pointer moves add up until they
    // cross half a grid step.
    private (GizmoOverlay.GizmoMode mode, GizmoOverlay.GizmoAxis axis) _dragKey;
    private float _dragAccum;
    private float _dragSnapped;

    private void OnGizmoDragStart(object? sender, System.EventArgs e)
    {
        _dragBefore = null;
        _dragApplied = false;
        _dragKey = (GizmoOverlay.GizmoMode.Translate, GizmoOverlay.GizmoAxis.None);
        _dragAccum = 0f;
        _dragSnapped = 0f;
    }

    private void OnGizmoDragEnd(object? sender, System.EventArgs e)
    {
        if (_vm != null && _dragApplied && _dragBefore is { Count: > 0 })
        {
            var after = new System.Collections.Generic.List<(Components.GameEntity, Common.TransformData)>();
            foreach (var (entity, _) in _dragBefore)
            {
                var t = entity.GetTransform();
                if (t != null) after.Add((entity, Common.TransformData.FromTransform(t)));
            }
            _vm.UndoRedo.Add(new Common.MultiEntityTransformAction(_dragBefore, after,
                $"Gizmo {_dragKey.mode}"));
        }
        _dragBefore = null;
        _dragApplied = false;
    }

    private void OnGizmoDrag(object? sender, (PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoMode mode, PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis axis, float delta) e)
    {
        if (_vm == null || _vm.SelectedEntities.Length == 0) return;
        var entities = _vm.SelectedEntities
            .Where(en => en.GetTransform() != null)
            .ToArray();
        if (entities.Length == 0) return;

        // Lazy capture: snapshot the pre-drag state once per drag, no
        // matter how many (mode, axis) switches happen within it.
        if (_dragBefore == null)
        {
            _dragBefore = entities
                .Select(en => (en, Common.TransformData.FromTransform(en.GetTransform()!)))
                .ToList();
        }

        // New gesture key (mode or axis changed mid-drag) resets the
        // snap accumulators.
        var key = (e.mode, e.axis);
        if (key != _dragKey)
        {
            _dragKey = key;
            _dragAccum = 0f;
            _dragSnapped = 0f;
        }

        var snap = _vm.SnapSettings;
        float delta = e.delta;

        if (e.mode == GizmoOverlay.GizmoMode.Translate && snap.EffectiveTranslateSnap(_ctrlDown))
        {
            _dragAccum += delta;
            var snapped = snap.SnapTranslateDelta(_dragAccum);
            delta = snapped - _dragSnapped;
            _dragSnapped = snapped;
        }
        else if (e.mode == GizmoOverlay.GizmoMode.Rotate && snap.EffectiveRotateSnap(_ctrlDown))
        {
            _dragAccum += delta;
            var snapped = snap.SnapRotateDelta(_dragAccum);
            delta = snapped - _dragSnapped;
            _dragSnapped = snapped;
        }
        else
        {
            _dragAccum += delta;
        }

        if (delta == 0f) return;
        _dragApplied = true;

        foreach (var entity in entities)
        {
            var transform = entity.GetTransform();
            if (transform == null) continue;

            var axis = e.axis;
            switch (e.mode)
            {
                case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoMode.Translate:
                    switch (axis)
                    {
                        case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis.X: transform.PositionX += delta; break;
                        case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis.Y: transform.PositionY += delta; break;
                        case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis.Z: transform.PositionZ += delta; break;
                    }
                    break;
                case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoMode.Rotate:
                    switch (axis)
                    {
                        case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis.X: transform.RotationX += delta; break;
                        case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis.Y: transform.RotationY += delta; break;
                        case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis.Z: transform.RotationZ += delta; break;
                    }
                    break;
                case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoMode.Scale:
                    float ClampScale(float v) => v < 0.01f ? 0.01f : (v > 100f ? 100f : v);
                    switch (axis)
                    {
                        case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis.X: transform.ScaleX = ClampScale(transform.ScaleX * delta); break;
                        case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis.Y: transform.ScaleY = ClampScale(transform.ScaleY * delta); break;
                        case PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis.Z: transform.ScaleZ = ClampScale(transform.ScaleZ * delta); break;
                    }
                    break;
            }
        }

        _vm.PushTransformsToNative(entities);
        _vm.Properties?.RefreshFromTransform(entities[0].GetTransform());
        _vm.MarkProjectDirty();
        var first = entities[0].GetTransform();
        if (first != null) _viewport?.SetGizmoTarget(first.PositionX, first.PositionY, first.PositionZ);
    }

    private void OnViewModelPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (_vm == null || _viewport == null) return;
        if (e.PropertyName == nameof(WorldEditorViewModel.ActiveScene))
        {
            _viewport.SetScene(_vm.ActiveScene);
            return;
        }
        if (e.PropertyName != nameof(WorldEditorViewModel.SelectedEntities)) return;
        SyncHierarchySelection();
        // Mirror the gizmo to the newly selected entity's world position
        // so the user can immediately drag the handles. If nothing is
        // selected, fall back to the world origin (matches the camera
        // pivot) so the gizmo is still visible / draggable in empty space.
        var sel = _vm.SelectedEntities;
        // Multi-select keeps the gizmo anchored on the first entity; drags
        // apply the same delta to every selected entity (plan §6.1 A2).
        if (sel.Length >= 1 && sel[0].GetTransform() is { } t)
        {
            _viewport.SetGizmoVisible(true);
            _viewport.SetGizmoTarget(t.PositionX, t.PositionY, t.PositionZ);
        }
        else
        {
            _viewport.SetGizmoVisible(false);
            _viewport.SetGizmoTarget(0f, 0f, 0f);
        }
    }

    private bool _syncingHierarchySelection;

    private void SyncHierarchySelection()
    {
        if (_vm == null || _syncingHierarchySelection) return;
        if (this.FindControl<TreeView>("HierarchyTree") is not { } tree) return;

        _syncingHierarchySelection = true;
        try
        {
            tree.SelectedItems.Clear();
            foreach (var entity in _vm.SelectedEntities)
                tree.SelectedItems.Add(entity);
            if (_vm.SelectedEntities.Length == 1)
                tree.ScrollIntoView(_vm.SelectedEntities[0]);
        }
        finally
        {
            _syncingHierarchySelection = false;
        }
    }

    private void OnEntityPicked(object? sender, uint nativeEntityId)
    {
        if (_vm == null || _vm.ActiveScene == null) return;
        // Find the C# GameEntity whose native id matches the pick.
        foreach (var e in _vm.ActiveScene.Entities)
        {
            if (_vm.GetNativeEntityId(e) == nativeEntityId)
            {
                _vm.SelectedEntities = new[] { e };
                // Sync the TreeView selection to the picked entity so
                // the user sees which row is highlighted, and scroll
                // the row into view if it is below the fold.
                if (this.FindControl<TreeView>("HierarchyTree") is { } tree)
                {
                    tree.SelectedItem = e;
                    tree.ScrollIntoView(e);
                }
                _viewport?.FocusOnEntity(nativeEntityId);
                return;
            }
        }
    }

    private void OnPickerCandidatesChanged(object? sender, System.EventArgs e) => RefreshPicker();

    private void RefreshPicker()
    {
        if (_viewport == null || _vm == null) return;
        _viewport.SetPickerCandidates(_vm.GetPickerCandidates());
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private void OnAddSceneClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm)
        {
            vm.AddScene();
        }
    }

    private void OnDeleteSceneClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm)
        {
            vm.RemoveScene(vm.ActiveScene);
        }
    }

    private void OnHierarchyFilterChanged(object? sender, Avalonia.Controls.TextChangedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        var filter = this.FindControl<Avalonia.Controls.TextBox>("HierarchyFilter")?.Text ?? "";
        vm.ApplyFilter(filter);
    }

    // =======================================================================
    // Hierarchy reparent drag & drop (plan §6.1 A3)
    // =======================================================================

    private Avalonia.Point? _hierarchyPressPoint;
    private Avalonia.Input.PointerPressedEventArgs? _hierarchyPressedArgs;
    private Common.HierarchyDragData? _hierarchyDragData;
    private const string HierarchyDragFormat = "PrimalEditor.HierarchyDrag";

    /// <summary>
    /// Wire the reparent drag: pointer press records the origin, pointer
    /// move past a small threshold starts a DragDrop carrying the current
    /// selection snapshot, and the TreeView's Drop event (wired in axaml)
    /// resolves the target item — drop on empty space = move to root.
    /// The entity snapshot rides in a view field; the transfer only
    /// carries the marker format (same-process drag, Avalonia 12).
    /// </summary>
    private void WireHierarchyDragDrop()
    {
        if (this.FindControl<TreeView>("HierarchyTree") is not { } tree) return;
        tree.PointerPressed += OnHierarchyPointerPressed;
        tree.PointerMoved += OnHierarchyPointerMoved;
    }

    private void OnHierarchyPointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        // Avalonia 12's DoDragDropAsync requires the original press args.
        _hierarchyPressedArgs = e;
        _hierarchyPressPoint = e.GetCurrentPoint((Visual)sender!).Properties.IsLeftButtonPressed
            ? e.GetPosition((Visual)sender!)
            : null;
    }

    private void OnHierarchyPointerMoved(object? sender, Avalonia.Input.PointerEventArgs e)
    {
        if (_hierarchyPressPoint == null || _hierarchyPressedArgs == null) return;
        if (!e.GetCurrentPoint((Visual)sender!).Properties.IsLeftButtonPressed)
        {
            _hierarchyPressPoint = null;
            _hierarchyPressedArgs = null;
            return;
        }
        var pos = e.GetPosition((Visual)sender!);
        var dx = pos.X - _hierarchyPressPoint.Value.X;
        var dy = pos.Y - _hierarchyPressPoint.Value.Y;
        if (dx * dx + dy * dy < 25) return; // 5px threshold
        if (DataContext is not WorldEditorViewModel vm) return;
        var entities = vm.SelectedEntities.Where(en => en != null).ToArray();
        if (entities.Length == 0) return;

        _hierarchyPressPoint = null;
        _hierarchyDragData = new Common.HierarchyDragData(entities);
        var pressedArgs = _hierarchyPressedArgs;
        _hierarchyPressedArgs = null;

        var format = Avalonia.Input.DataFormat.CreateStringApplicationFormat(HierarchyDragFormat);
        var transfer = new Avalonia.Input.DataTransfer();
        transfer.Add(Avalonia.Input.DataTransferItem.Create(format, HierarchyDragFormat));
        var dragged = _hierarchyDragData;
        _ = Avalonia.Input.DragDrop.DoDragDropAsync(pressedArgs, transfer, Avalonia.Input.DragDropEffects.Move)
            .ContinueWith(_ =>
            {
                // Cancelled or dropped outside the tree: clear the payload.
                if (ReferenceEquals(_hierarchyDragData, dragged)) _hierarchyDragData = null;
            }, System.Threading.Tasks.TaskScheduler.FromCurrentSynchronizationContext());
    }

    private void OnHierarchyDragOver(object? sender, Avalonia.Input.DragEventArgs e)
    {
        var format = Avalonia.Input.DataFormat.CreateStringApplicationFormat(HierarchyDragFormat);
        e.DragEffects = _hierarchyDragData != null && e.DataTransfer.Contains(format)
            ? Avalonia.Input.DragDropEffects.Move
            : Avalonia.Input.DragDropEffects.None;
    }

    private void OnHierarchyDrop(object? sender, Avalonia.Input.DragEventArgs e)
    {
        var format = Avalonia.Input.DataFormat.CreateStringApplicationFormat(HierarchyDragFormat);
        if (DataContext is not WorldEditorViewModel vm
            || _hierarchyDragData is not { } drag
            || !e.DataTransfer.Contains(format))
        {
            return;
        }
        _hierarchyDragData = null;

        var targetParent = ResolveHierarchyDropTarget(sender, e);
        foreach (var entity in drag.Entities)
        {
            if (entity == null) continue;
            if (!vm.ReparentEntity(entity, targetParent))
            {
                Logger.Info($"[Hierarchy] cannot reparent '{entity.Name}' under " +
                            (targetParent?.Name ?? "scene root"));
                continue;
            }
            vm.UndoRedo.Add(new Common.ReparentEntityAction(vm, entity, targetParent));
            Logger.Info($"[Hierarchy] '{entity.Name}' reparented under " +
                        (targetParent?.Name ?? "scene root"));
        }
        e.Handled = true;
    }

    /// <summary>Hit-test the drop point to a TreeViewItem's GameEntity;
    /// falling back to null (scene root) when the pointer is over empty
    /// tree space.</summary>
    private GameEntity? ResolveHierarchyDropTarget(object? sender, Avalonia.Input.DragEventArgs e)
    {
        if (sender is not Avalonia.Input.IInputElement input) return null;
        if (input.InputHitTest(e.GetPosition(sender as Visual ?? this)) is not Visual hit) return null;
        for (var v = hit; v != null; v = Avalonia.VisualTree.VisualExtensions.GetVisualParent(v))
        {
            if (v is Avalonia.Controls.TreeViewItem { DataContext: GameEntity ge })
                return ge;
        }
        return null;
    }

    private void OnTreeSelectionChanged(object? sender, SelectionChangedEventArgs e)
    {
        if (_syncingHierarchySelection) return;
        if (DataContext is WorldEditorViewModel vm && sender is TreeView treeView)
        {
            var selected = treeView.SelectedItems.OfType<GameEntity>().ToArray();
            vm.SelectedEntities = selected;
            if (vm.Properties != null)
            {
                vm.Properties.Entities = selected;
                // Tell the Properties VM the native id of the single
                // selected entity so it can update the look-through
                // button label ("Look Through This Camera" vs.
                // "Restore Editor Camera"). 0 when nothing or multi.
                uint nativeId = selected.Length == 1
                    ? vm.GetNativeEntityId(selected[0])
                    : 0u;
                vm.Properties.RefreshEntityContext(nativeId);
            }
        }
    }

    private async void OnCreatePrimitiveMeshClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        var dlg = new Editors.PrimitiveMesh.PrimitiveMeshView
        {
            DataContext = new Editors.PrimitiveMesh.PrimitiveMeshViewModel(),
        };
        var window = TopLevel.GetTopLevel(this) as Window;
        if (window != null) await dlg.ShowDialog<bool?>(window);
    }

    private async void OnAddEntityKindClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (sender is not Avalonia.Controls.Button { Tag: EntityFactoryDescriptor descriptor }) return;
        if (DataContext is not WorldEditorViewModel vm) return;

        if (descriptor.RequiresAssetSelection)
        {
            // Prop / Asset-selected kinds open a file picker filtered to
            // mesh formats. The user picks an asset, the picker flows
            // through DropAssetAsync which creates the entity and binds
            // it to the chosen mesh GUID. No special case in the
            // descriptor beyond RequiresAssetSelection = true.
            var topLevel = TopLevel.GetTopLevel(this);
            if (topLevel?.StorageProvider is { } storage)
            {
                var filters = new System.Collections.Generic.List<Avalonia.Platform.Storage.FilePickerFileType>
                {
                    new Avalonia.Platform.Storage.FilePickerFileType("Mesh assets")
                    {
                        Patterns = new System.Collections.Generic.List<string>
                        {
                            "*.mesh", "*.obj", "*.fbx", "*.gltf", "*.glb"
                        }
                    }
                };
                var options = new Avalonia.Platform.Storage.FilePickerOpenOptions
                {
                    Title = $"Select a mesh for {descriptor.DisplayName}",
                    AllowMultiple = false,
                    FileTypeFilter = filters
                };
                var files = await storage.OpenFilePickerAsync(options);
                if (files == null || files.Count == 0) return;
                var picked = files[0];
                var path = picked.Path.LocalPath;
                var info = PrimalEditor_Avalonia.Content.AssetRegistry.Get(path) ?? new PrimalEditor_Avalonia.Content.AssetInfo
                {
                    FullPath = path,
                    Type = PrimalEditor_Avalonia.Content.ContentHelper.GetAssetType(path)
                };
                try { await vm.DropAssetAsync(info, 0f, 0f, 0f); }
                catch (System.Exception ex) { PrimalEditor_Avalonia.Common.Logger.Error($"{descriptor.DisplayName} drop failed", ex); }
                return;
            }
        }

        // The Character/Light/Camera/Empty kinds go through the standard
        // factory path: descriptor.CreateEntity() + parent under selection
        // + SceneSynchronizer.SyncEntity (the same flow as the legacy
        // "+ Entity" button, just driven by the descriptor instead of
        // a hard-coded Transform).
        vm.CreateEntityFromFactory(descriptor);
    }

    private void OnAddEntityClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm)
        {
            var descriptor = EntityFactory.Find(EntityKind.Empty);
            if (descriptor != null) vm.CreateEntityFromFactory(descriptor);
        }
    }

    private void OnDeleteEntityClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
		if (DataContext is WorldEditorViewModel vm)
        {
			var count = vm.DeleteSelectedEntities();
			if (count > 0) Logger.Info($"Deleted {count} selected entity(ies)");
        }
    }

    /// <summary>
    /// A8 源码控制: open the (singleton) source-control panel bound to the
    /// current project folder. The panel degrades gracefully outside a git
    /// work tree.
    /// </summary>
    private void OnSourceControlClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        if (TopLevel.GetTopLevel(this) is not Window window) return;
        SourceControlPanel.ShowFor(window, vm.Project?.Path);
    }

    private async void OnArrayDuplicateClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        if (vm.ActiveScene == null || vm.SelectedEntities.Length == 0)
        {
            Logger.Info("[Array] select at least one entity first");
            return;
        }
        if (TopLevel.GetTopLevel(this) is not Window window) return;
        var dialog = new Views.ArrayDuplicateDialog();
        if (await dialog.ShowDialog<bool?>(window) != true || !dialog.Confirmed) return;
        vm.ArrayDuplicateSelected(dialog.Axis, dialog.Count, dialog.Offset);
    }

    /// <summary>
    /// 给选中实体挂 Environment 组件——属性面板的 ENVIRONMENT 区块
    /// （雾 / probe 调试 / GI 体积 / Skybox）仅对该实体显示。
    /// </summary>
    private void OnAddEnvironmentClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        if (vm.SelectedEntities.Length == 0)
        {
            Logger.Info("[Environment] select an entity first");
            return;
        }
        var target = vm.SelectedEntities[0];
        if (target.GetComponent<Components.EnvironmentComponent>() != null)
        {
            Logger.Info($"[Environment] '{target.Name}' already has an environment component");
            return;
        }
        target.Components.Add(new Components.EnvironmentComponent());
        if (vm.GetNativeEntityId(target) is var nativeId && nativeId != 0)
        {
            vm.SceneSynchronizer.SyncEntity(target);
        }
        vm.MarkProjectDirty();
        Logger.Info($"[Environment] component added to '{target.Name}'");
    }

    private void OnAddParticleEmitterClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        if (vm.SelectedEntities.Length == 0)
        {
            Logger.Info("[Particle] select an entity first");
            return;
        }
        // v1：给首个选中实体挂组件；组件级 undo 与移除路径随 P2 补齐。
        var target = vm.SelectedEntities[0];
        if (target.GetComponent<Components.ParticleEmitterComponent>() != null)
        {
            Logger.Info($"[Particle] '{target.Name}' already has an emitter");
            return;
        }
        target.Components.Add(new Components.ParticleEmitterComponent());
        if (vm.GetNativeEntityId(target) is var nativeId && nativeId != 0)
        {
            vm.SceneSynchronizer.SyncEntity(target);
        }
        vm.MarkProjectDirty();
        Logger.Info($"[Particle] emitter component added to '{target.Name}'");
    }

    private async void OnCreatePrefabClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        if (vm.Project == null || vm.ActiveScene == null || vm.SelectedEntities.Length == 0)
        {
            Logger.Info("[Prefab] select at least one entity first");
            return;
        }
        if (TopLevel.GetTopLevel(this) is not Window window) return;

        var dialog = new Views.TextInputDialog(
            "Create Prefab", "Prefab name:", vm.SelectedEntities[0].Name);
        var name = await dialog.ShowDialog<string?>(window);
        if (string.IsNullOrWhiteSpace(name)) return;

        var safeName = string.Join('_', name.Split(
            System.IO.Path.GetInvalidFileNameChars(), StringSplitOptions.RemoveEmptyEntries));
        var documents = PrimalEditor_Avalonia.GameProject.Serializer.ToEntityDocuments(vm.SelectedEntities);
        if (documents.Count == 0) return;

        var prefab = new PrimalEditor_Avalonia.Content.Prefabs.PrefabDocument
        {
            Name = name,
            Guid = new PrimalEditor_Avalonia.Content.AssetGuid(Guid.NewGuid()),
            Entities = documents,
        };
        var path = System.IO.Path.Combine(vm.Project.Path, safeName + ".prefab");
        PrimalEditor_Avalonia.Content.Prefabs.PrefabSerializer.Write(path, prefab);
        Logger.Info($"[Prefab] created {path} ({documents.Count} entity docs) — ContentWatcher will index it");
    }

    private async void OnEntityDoubleClick(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm
            && this.FindControl<TreeView>("HierarchyTree") is { } tree
            && tree.SelectedItem is GameEntity entity)
        {
            var dialog = new RenameEntityDialog(entity.Name);
            var topLevel = TopLevel.GetTopLevel(this);
            if (topLevel is Window window)
            {
                var result = await dialog.ShowDialog<string?>(window);
                if (!string.IsNullOrEmpty(result) && result != entity.Name)
                {
                    var action = new RenameEntityAction(entity, result);
                    vm.UndoRedo.Add(action);
                    action.Redo();
                    Logger.Info($"Renamed entity to: {result}");
                }
            }
        }
    }

    private async void OnAssetDrop(object? sender, Avalonia.Input.DragEventArgs e)
    {
        e.Handled = true;
        var vm = DataContext as WorldEditorViewModel;
        await AssetDropHandler.RunAsync(e, vm, 0f, 0f, 0f);
    }

    private void OnDragOver(object? sender, Avalonia.Input.DragEventArgs e)
    {
        e.DragEffects = AssetDropHandler.EffectsFor(e, DataContext as WorldEditorViewModel);
    }

    private void OnOrthoToggleClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (this.FindControl<PrimalEditor_Avalonia.Views.Engine3D.EngineViewport3D>("WorldViewport")
            is PrimalEditor_Avalonia.Views.Engine3D.EngineViewport3D viewport)
        {
            viewport.IsOrthographic = !viewport.IsOrthographic;
        }
    }

    private void OnTranslateModeClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e) =>
        _viewport?.SetGizmoMode(GizmoOverlay.GizmoMode.Translate);

    private void OnRotateModeClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e) =>
        _viewport?.SetGizmoMode(GizmoOverlay.GizmoMode.Rotate);

    private void OnScaleModeClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e) =>
        _viewport?.SetGizmoMode(GizmoOverlay.GizmoMode.Scale);

    private void OnBuildClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        var config = vm.Project?.BuildConfigName ?? "Debug";
        AppendBuildLog($"[build] starting config={config}");
        IsBuilding = true;
        BuildService.Start(config);
    }

    private void OnRebuildClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        var config = vm.Project?.BuildConfigName ?? "Debug";
        AppendBuildLog($"[rebuild] starting config={config}");
        IsBuilding = true;
        BuildService.StartRebuild(config);
    }

    private void OnRunClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        var config = vm.Project?.BuildConfigName ?? "Debug";
        AppendBuildLog($"[run] launching PrimalPlayer (api={vm.TargetGraphicsApi}, config={config})");
        IsBuilding = true;
        // Run 的新语义（规划 §Run/Stop 2026-09-03）：导出运行时场景包并启动
        // PrimalPlayer 外部宿主（不再是"重启编辑器自己"）。Stop 复用同一进程句柄。
        if (!RuntimeLauncher.Run(vm, config))
        {
            AppendBuildLog("[run] launch failed — check log (missing player binary or no scene)");
            IsBuilding = false;
        }
    }

    private void OnBuildStopClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        AppendBuildLog("[build] stop requested");
        BuildService.Stop();
    }

    // BuildLogView (the inner control at the bottom of WorldEditorView's
    // tab strip) subscribes to BuildService itself. Toolbar buttons call
    // AppendBuildLog to record their user-action lines alongside the
    // captured stdout / exit-code entries.
    private void AppendBuildLog(string line)
    {
        if (this.FindControl<PrimalEditor_Avalonia.Views.BuildLogView>(null) is not { } logView) return;
        if (logView.FindControl<Avalonia.Controls.TextBox>("BuildLogText") is not Avalonia.Controls.TextBox textBox) return;
        textBox.Text += (textBox.Text.Length > 0 ? "\n" : string.Empty) + line;
    }

    private void OnWorldEditorKeyDown(object? sender, Avalonia.Input.KeyEventArgs e)
    {
		if ((e.Key == Avalonia.Input.Key.Delete || e.Key == Avalonia.Input.Key.Back) &&
			e.Source is not Avalonia.Controls.TextBox)
		{
			if (DataContext is WorldEditorViewModel deleteVm && deleteVm.DeleteSelectedEntities() > 0)
			{
				e.Handled = true;
			}
			return;
		}
        if (e.Key == Avalonia.Input.Key.W)
        {
            _viewport?.SetGizmoMode(GizmoOverlay.GizmoMode.Translate);
            e.Handled = true;
            return;
        }
        if (e.Key == Avalonia.Input.Key.E)
        {
            _viewport?.SetGizmoMode(GizmoOverlay.GizmoMode.Rotate);
            e.Handled = true;
            return;
        }
        if (e.Key == Avalonia.Input.Key.R)
        {
            _viewport?.SetGizmoMode(GizmoOverlay.GizmoMode.Scale);
            e.Handled = true;
            return;
        }
        // F = focus the selected entity. When no entity is selected,
        // do nothing — the focused child (e.g. EngineViewport3D) will
        // handle the F key for its own "reset view" semantics.
        if (e.Key != Avalonia.Input.Key.F) return;
        if (DataContext is not WorldEditorViewModel vm) return;
        if (vm.SelectedEntities.Length == 0) return;
        var nativeId = vm.GetNativeEntityId(vm.SelectedEntities[0]);
        if (nativeId == 0) return;
        _viewport?.FocusOnEntity(nativeId);
        e.Handled = true;
    }
}
