using System.Linq;
using Avalonia;
using Avalonia.Controls;
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
            BuildService.OnOutput += OnBuildOutput;
            BuildService.OnCompleted += OnBuildCompleted;
            Unloaded += (_, _) =>
            {
                BuildService.OnOutput -= OnBuildOutput;
                BuildService.OnCompleted -= OnBuildCompleted;
            };
        }

        private EngineViewport3D? _viewport;
        private WorldEditorViewModel? _vm;

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
        }
        RefreshPicker();
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
        // Mirror the gizmo to the newly selected entity's world position
        // so the user can immediately drag the handles. If nothing is
        // selected, fall back to the world origin (matches the camera
        // pivot) so the gizmo is still visible / draggable in empty space.
        var sel = _vm.SelectedEntities;
        if (sel.Length == 1 && sel[0].GetTransform() is { } t)
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

    private void OnGizmoDrag(object? sender, (PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoMode mode, PrimalEditor_Avalonia.Views.Engine3D.GizmoOverlay.GizmoAxis axis, float delta) e)
    {
        if (_vm == null || _vm.SelectedEntities.Length == 0) return;
        var entity = _vm.SelectedEntities[0];
        var transform = entity.GetTransform();
        if (transform == null) return;

        var axis = e.axis;
        var delta = e.delta;

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

        _vm.PushTransformsToNative(new[] { entity });
        _vm.Properties?.RefreshFromTransform(transform);
        _vm.MarkProjectDirty();
        _viewport?.SetGizmoTarget(transform.PositionX, transform.PositionY, transform.PositionZ);
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

    private void OnTreeSelectionChanged(object? sender, SelectionChangedEventArgs e)
    {
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
            if (vm.ActiveScene == null)
            {
                Logger.Warning("Cannot add entity: no active scene. Open a project first.");
                return;
            }

            var parent = vm.SelectedEntities.Length == 1 ? vm.SelectedEntities[0] : null;
            var entityName = parent != null
                ? $"{parent.Name}_Child_{parent.Children.Count + 1}"
                : $"Entity_{vm.Entities.Count + 1}";

            var entity = new GameEntity(entityName);
            entity.Components.Add(new Transform());

            if (parent != null)
            {
                parent.Children.Add(entity);
                entity.Parent = parent;
            }
            else
            {
                vm.ActiveScene.Entities.Add(entity);
                entity.ParentScene = vm.ActiveScene;
            }

            vm.UndoRedo.Add(new AddEntityAction(vm, entity));
            vm.SelectedEntities = new[] { entity };
            // Mirror the toolbar path: push the entity into the native
            // engine so the editor viewport sees it. Without this the
            // hierarchy shows the entity but the render surface stays
            // empty.
            if (PrimalEditor_Avalonia.Native.NativeEngineBridge.IsLoaded)
            {
                vm.SceneSynchronizer.SyncEntity(entity);
            }
            Logger.Info($"Added entity: {entity.Name}" + (parent != null ? $" (child of {parent.Name})" : ""));
        }
    }

    private void OnDeleteEntityClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm && vm.SelectedEntities.Length > 0)
        {
            int count = 0;
            foreach (var entity in vm.SelectedEntities.ToList())
            {
                CountRecursive(entity, ref count);
                RemoveRecursive(entity);
                vm.UndoRedo.Add(new RemoveEntityAction(vm, entity));
                ((RemoveEntityAction)vm.UndoRedo.UndoStack[^1]).Redo();
            }
            vm.SelectedEntities = System.Array.Empty<GameEntity>();
            Logger.Info($"Deleted {count} entity(ies) (including children)");
        }
    }

    private int CountRecursive(GameEntity entity, ref int count)
    {
        count++;
        foreach (var child in entity.Children)
        {
            CountRecursive(child, ref count);
        }
        return count;
    }

    private void RemoveRecursive(GameEntity entity)
    {
        if (entity.Parent != null)
        {
            entity.Parent.Children.Remove(entity);
        }
        else if (entity.ParentScene != null)
        {
            entity.ParentScene.Entities.Remove(entity);
        }
        entity.Parent = null;
        entity.ParentScene = null;
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
        if (DataContext is not WorldEditorViewModel vm) return;
        if (vm.ActiveScene == null) return;

        var files = e.Data.Contains(Avalonia.Input.DataFormats.Files)
            ? e.Data.Get(Avalonia.Input.DataFormats.Files) as System.Collections.IEnumerable
            : null;

        string? droppedPath = null;
        if (files != null)
        {
            foreach (var f in files)
            {
                if (f is string s) { droppedPath = s; break; }
                if (f is Avalonia.Platform.Storage.IStorageItem item && item.Path.IsFile)
                {
                    droppedPath = item.Path.LocalPath;
                    break;
                }
            }
        }
        if (string.IsNullOrEmpty(droppedPath) || !System.IO.File.Exists(droppedPath))
        {
            return;
        }

        // Find the .meta sidecar's AssetInfo (it was registered when the
        // .mesh was cooked). If not, create a temporary AssetInfo from
        // the path so the drop pipeline can still proceed.
        var info = PrimalEditor_Avalonia.Content.AssetRegistry.Get(droppedPath) ?? new PrimalEditor_Avalonia.Content.AssetInfo
        {
            FullPath = droppedPath,
            Type = PrimalEditor_Avalonia.Content.ContentHelper.GetAssetType(droppedPath),
        };
        try
        {
            await vm.DropAssetAsync(info, 0f, 0f, 0f);
        }
        catch (System.Exception ex)
        {
            Logger.Error("DropAsset failed", ex);
        }
    }

    private void OnDragOver(object? sender, Avalonia.Input.DragEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm && vm.ActiveScene != null)
        {
            e.DragEffects = Avalonia.Input.DragDropEffects.Copy;
        }
        else
        {
            e.DragEffects = Avalonia.Input.DragDropEffects.None;
        }
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
        var config = vm.Project?.BuildConfigName ?? "DebugEditor";
        AppendBuildLog($"[build] starting config={config}");
        IsBuilding = true;
        BuildService.Start(config);
    }

    private void OnRebuildClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        var config = vm.Project?.BuildConfigName ?? "DebugEditor";
        AppendBuildLog($"[rebuild] starting config={config}");
        IsBuilding = true;
        BuildService.StartRebuild(config);
    }

    private void OnRunClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        var config = vm.Project?.BuildConfigName ?? "DebugEditor";
        AppendBuildLog($"[run] launching app (config={config})");
        if (!BuildService.StartApp(config))
        {
            AppendBuildLog("[run] failed to launch — check script path and console");
        }
    }

    private void OnBuildLogDoubleClick(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        if (this.FindControl<Avalonia.Controls.TextBox>("BuildLogText") is not Avalonia.Controls.TextBox logBox) return;
        // Get the line under the cursor (or use selected text if any).
        var selected = logBox.SelectedText;
        if (string.IsNullOrEmpty(selected)) selected = logBox.Text ?? string.Empty;
        var hit = BuildErrorParser.Parse(selected);
        if (hit == null)
        {
            AppendBuildLog("[jump] no file:line:col pattern in selection");
            return;
        }
        AppendBuildLog($"[jump] {hit.Value.File}:{hit.Value.Line}:{hit.Value.Column} (file exists: {System.IO.File.Exists(hit.Value.File)})");
        // TODO: integrate with native OpenInEditor once the engine
        // exposes that API. For now we surface the parsed location
        // in the log; users can right-click → Reveal in Finder.
    }

    private void OnBuildStopClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        AppendBuildLog("[build] stop requested");
        BuildService.Stop();
    }

    private void OnBuildOutput(string line)
    {
        Avalonia.Threading.Dispatcher.UIThread.Post(() => AppendBuildLog(line));
    }

    private void OnBuildCompleted(int exitCode, bool success)
    {
        Avalonia.Threading.Dispatcher.UIThread.Post(() =>
        {
            AppendBuildLog($"[build] exit code={exitCode} success={success}");
            IsBuilding = false;
            if (DataContext is WorldEditorViewModel vm)
            {
                vm.DebugMessage = success ? "Build succeeded" : $"Build failed (exit {exitCode})";
            }
        });
    }

    private void AppendBuildLog(string line)
    {
        var textBox = this.FindControl<Avalonia.Controls.TextBox>("BuildLogText");
        if (textBox == null) return;
        textBox.Text += (textBox.Text.Length > 0 ? "\n" : string.Empty) + line;
    }

    private void OnWorldEditorKeyDown(object? sender, Avalonia.Input.KeyEventArgs e)
    {
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
