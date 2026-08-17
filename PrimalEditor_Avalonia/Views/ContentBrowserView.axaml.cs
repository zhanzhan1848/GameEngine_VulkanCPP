using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class ContentBrowserView : UserControl
{
    private const double DragThreshold = 5.0;

    private AssetInfo? _dragCandidate;
    private Guid? _dragCandidateParentGuid;
    private Point _dragStart;
    private bool _toggleContainerOnRelease;
    private bool _dragInProgress;

    public ContentBrowserView()
    {
        InitializeComponent();
        KeyDown += OnContentBrowserKeyDown;
        PointerMoved += OnAssetPointerMoved;
        PointerReleased += OnAssetPointerReleased;
    }

    private void OnFolderDoubleClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is ContentBrowserViewModel vm)
        {
            vm.LoadFilesForSelectedFolder();
        }
    }

    private void OnBreadcrumbClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not ContentBrowserViewModel vm) return;
        if (sender is Avalonia.Controls.Button btn && btn.Tag is BreadcrumbSegment seg)
        {
            vm.NavigateToBreadcrumb(seg);
        }
    }

    private void OnClearSearchClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is ContentBrowserViewModel vm) vm.ClearSearch();
        SearchBox?.Focus();
    }

    private void OnSearchKeyDown(object? sender, Avalonia.Input.KeyEventArgs e)
    {
        if (e.Key == Avalonia.Input.Key.Escape && DataContext is ContentBrowserViewModel vm)
        {
            vm.ClearSearch();
            e.Handled = true;
        }
    }

    private void OnContentBrowserKeyDown(object? sender, Avalonia.Input.KeyEventArgs e)
    {
        // Cmd+F (macOS) / Ctrl+F (Windows/Linux) — focus the search box.
        var modifiers = e.KeyModifiers;
        bool hasCmdOrCtrl =
            (modifiers & Avalonia.Input.KeyModifiers.Meta) != 0 ||
            (modifiers & Avalonia.Input.KeyModifiers.Control) != 0;
        if (hasCmdOrCtrl && e.Key == Avalonia.Input.Key.F)
        {
            SearchBox?.Focus();
            SearchBox?.SelectAll();
            e.Handled = true;
        }
    }

    private void OnFileDoubleClick(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        if (DataContext is not ContentBrowserViewModel vm) return;
        if (vm.SelectedFile is AssetInfo info)
        {
            // Submesh children have synthetic paths; double-click is
            // equivalent to single-select in OnFileSelectionChanged.
            // The actual preview was already wired up by the selection
            // event, so there's nothing further to do here.
            if (!info.IsContainer && FindParentOf(info) != null) return;

            // Folder entry: navigate into it the same way the left
            // tree handles double-click. Update SelectedFolder and
            // breadcrumb, then reload the file list.
            if (info.IsFolder && Directory.Exists(info.FullPath))
            {
                NavigateToFolder(info.FullPath);
                e.Handled = true;
                return;
            }

            if (AssetEditorRegistry.Open(info) == null)
                vm.OpenSelectedFile();
        }
        else
        {
            vm.OpenSelectedFile();
        }
    }

    private void NavigateToFolder(string folderPath)
    {
        if (DataContext is not ContentBrowserViewModel vm) return;
        // Find the matching ContentFolder in the existing Folders tree
        // (so breadcrumbs and the left tree selection stay in sync),
        // or fall back to a temporary folder entry if none exists.
        var folder = FindFolderByPath(vm.Folders, folderPath);
        if (folder != null)
        {
            vm.SelectedFolder = folder;
        }
        else
        {
            // Manually trigger a refresh by setting SelectedFolder to
            // null first (forces a clean state) then to the new folder.
            vm.SelectedFolder = null;
            var stub = new ContentFolder { Name = Path.GetFileName(folderPath), FullPath = folderPath };
            vm.SelectedFolder = stub;
        }
    }

    private static ContentFolder? FindFolderByPath(IEnumerable<ContentFolder> folders, string fullPath)
    {
        foreach (var f in folders)
        {
            if (string.Equals(f.FullPath, fullPath, StringComparison.OrdinalIgnoreCase)) return f;
            var nested = FindFolderByPath(f.Subfolders, fullPath);
            if (nested != null) return nested;
        }
        return null;
    }

    private void OnFileSelectionChanged(object? sender, Avalonia.Controls.SelectionChangedEventArgs e)
    {
        if (DataContext is not ContentBrowserViewModel vm) return;
        // Pull the new selection from the event args. The vm.SelectedFile
        // binding may not have propagated yet when this handler runs,
        // so e.AddedItems is the only reliable source for the new item.
        // A quirk of TreeView is that the items themselves may come
        // through wrapped in a node container, so we accept the first
        // AssetInfo we find among the added items.
        AssetInfo? selected = null;
        if (e.AddedItems != null)
        {
            foreach (var item in e.AddedItems)
            {
                if (item is AssetInfo ai) { selected = ai; break; }
            }
        }
        // Fall back to vm.SelectedFile if the event args don't carry
        // anything useful (e.g. keyboard navigation that doesn't change
        // the AddedItems list).
        selected ??= vm.SelectedFile;
        if (selected == null) return;

        // Submesh selection: find the parent first so the auto-collapse
        // pass below can preserve the parent's expansion.
        AssetInfo? parentForCollapse = null;
        if (!selected.IsContainer)
        {
            parentForCollapse = FindParentOf(selected);
        }

        // Auto-collapse every previously-expanded container except
        // (a) the one we just clicked, and (b) the parent of a submesh
        // click — submesh tiles must stay visible while previewing the
        // submesh. The user expects only the most recently touched
        // container to be expanded at a time.
        foreach (var info in AssetRegistry.Assets)
        {
            if (!info.IsContainer) continue;
            if (info == selected) continue;
            if (info == parentForCollapse) continue;
            if (info.IsExpanded) info.IsExpanded = false;
        }

        if (selected.IsContainer)
        {
            // Selecting a model previews it. Expansion is controlled by the
            // tile/arrow handlers so keyboard selection and the arrow button
            // cannot accidentally toggle the state twice.
            vm.PreviewViewModel.SetAssetAndRefresh(selected);
            vm.PreviewViewModel.SelectedSubmeshIndex = 0;
            return;
        }

        // Submesh child: forward to the preview VM as (parent,
        // submesh_index). The preview VM's LoadMesh then refreshes
        // the wireframe to the parent geometry.
        var parent = parentForCollapse ?? FindParentOf(selected);
        if (parent != null)
        {
            var idx = parent.Children.IndexOf(selected);
            if (idx >= 0)
            {
                vm.SelectSubmesh(parent, idx);
                return;
            }
        }

        // Ordinary asset (texture, material, script, .fbx, etc.): preview
        // directly. Folders are excluded — they have no previewable
        // content and double-click navigates into them instead.
        if (selected.IsFolder) return;
        vm.PreviewViewModel.Asset = selected;
    }

    private void OnContainerTilePointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        // The arrow is a real Button and owns its click. Do not also toggle
        // from the containing tile when that button originated the event.
        if (e.Source is Button) return;
        if (sender is Control { DataContext: AssetInfo info })
        {
            BeginPotentialAssetDrag(e, info, parentGuid: null, toggleContainerOnRelease: true);
        }
    }

    private void OnAssetItemPointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        if (sender is Control { DataContext: AssetInfo info })
        {
            BeginPotentialAssetDrag(e, info, parentGuid: null, toggleContainerOnRelease: false);
        }
    }

    private void OnSubmeshItemPointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        if (sender is Control { DataContext: AssetInfo info })
        {
            var parentGuid = FindParentOf(info)?.Guid;
            BeginPotentialAssetDrag(e, info, parentGuid, toggleContainerOnRelease: false);
        }
    }

    private void BeginPotentialAssetDrag(
        Avalonia.Input.PointerPressedEventArgs e,
        AssetInfo info,
        Guid? parentGuid,
        bool toggleContainerOnRelease)
    {
        _dragCandidate = info.IsFolder ? null : info;
        _dragCandidateParentGuid = parentGuid;
        _dragStart = e.GetPosition(this);
        _toggleContainerOnRelease = toggleContainerOnRelease;
    }

    private async void OnAssetPointerMoved(object? sender, Avalonia.Input.PointerEventArgs e)
    {
        if (_dragCandidate == null || _dragInProgress) return;
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
        {
            ClearPotentialAssetDrag();
            return;
        }

        var current = e.GetPosition(this);
        if (!HasExceededDragThreshold(_dragStart, current)) return;

        var info = _dragCandidate;
        var parentGuid = _dragCandidateParentGuid;
        ClearPotentialAssetDrag();
        _dragInProgress = true;

        try
        {
            var payload = new AssetDragPayload(info.Guid, info.FullPath, info.IsContainer, parentGuid);
            var format = Avalonia.Input.DataFormat.CreateStringApplicationFormat(AssetDragPayload.DataFormat);
            var drag = new Avalonia.Input.DataTransfer();
            drag.Add(Avalonia.Input.DataTransferItem.Create(format, payload.Serialize()));
            await Avalonia.Input.DragDrop.DoDragDropAsync(e, drag, Avalonia.Input.DragDropEffects.Copy);
        }
        catch (Exception ex)
        {
            PrimalEditor_Avalonia.Common.Logger.Error("Asset drag failed", ex);
        }
        finally
        {
            _dragInProgress = false;
        }
    }

    private void OnAssetPointerReleased(object? sender, Avalonia.Input.PointerReleasedEventArgs e)
    {
        var container = _toggleContainerOnRelease ? _dragCandidate : null;
        ClearPotentialAssetDrag();
        if (container != null) ToggleContainer(container);
    }

    internal static bool HasExceededDragThreshold(Point start, Point current) =>
        Math.Abs(current.X - start.X) >= DragThreshold ||
        Math.Abs(current.Y - start.Y) >= DragThreshold;

    private void ClearPotentialAssetDrag()
    {
        _dragCandidate = null;
        _dragCandidateParentGuid = null;
        _toggleContainerOnRelease = false;
    }

    private void OnContainerToggleClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (sender is Button { Tag: AssetInfo info })
            ToggleContainer(info);
        e.Handled = true;
    }

    private void ToggleContainer(AssetInfo info)
    {
        if (DataContext is not ContentBrowserViewModel vm) return;

        foreach (var candidate in AssetRegistry.Assets)
        {
            if (candidate.IsContainer && candidate != info && candidate.IsExpanded)
                candidate.IsExpanded = false;
        }

        info.IsExpanded = !info.IsExpanded;
        vm.SelectedFile = info;
        vm.PreviewViewModel.SetAssetAndRefresh(info);
        vm.PreviewViewModel.SelectedSubmeshIndex = 0;
    }

    private static AssetInfo? FindParentOf(AssetInfo child)
    {
        // Walk the global AssetRegistry for any model container whose
        // Children list contains this child reference. The legacy
        // Application.Current.DataContext lookup used to live here
        // but it's typically null, so the lookup always returned null
        // and submesh clicks never reached the parent preview path.
        foreach (var info in AssetRegistry.Assets)
        {
            if (info.IsContainer && info.Children.Contains(child)) return info;
        }
        return null;
    }

    private void OnDragEnter(object? sender, Avalonia.Input.DragEventArgs e)
    {
        if (!HasDroppedFiles(e))
        {
            e.DragEffects = Avalonia.Input.DragDropEffects.None;
            return;
        }
        e.DragEffects = Avalonia.Input.DragDropEffects.Copy;
        if (DropOverlay != null) DropOverlay.IsVisible = true;
    }

    private void OnDragLeave(object? sender, Avalonia.Input.DragEventArgs e)
    {
        if (DropOverlay != null) DropOverlay.IsVisible = false;
    }

    private void OnDragOver(object? sender, Avalonia.Input.DragEventArgs e)
    {
        e.DragEffects = HasDroppedFiles(e)
            ? Avalonia.Input.DragDropEffects.Copy
            : Avalonia.Input.DragDropEffects.None;
    }

    private async void OnFileDrop(object? sender, Avalonia.Input.DragEventArgs e)
    {
        if (DropOverlay != null) DropOverlay.IsVisible = false;

        // Internal Content Browser asset drags use AssetDragPayload, not
        // DataFormat.File. They may be released over this panel and must not
        // enter the external-file import path.
        if (!HasDroppedFiles(e)) return;

        if (DataContext is not ContentBrowserViewModel vm) return;
        if (vm.Project == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning("Cannot drop files: no project loaded");
            return;
        }

        var paths = new System.Collections.Generic.List<string>();
        var files = e.DataTransfer.TryGetFiles();
        if (files != null)
        {
            foreach (var f in files)
            {
                if (f is Avalonia.Platform.Storage.IStorageItem item && item.Path.IsFile)
                    paths.Add(item.Path.LocalPath);
            }
        }

        if (paths.Count == 0) return;

        var ownerWindow = Avalonia.Controls.TopLevel.GetTopLevel(this) as Avalonia.Controls.Window;
        if (ownerWindow == null) return;
        try
        {
            await vm.ImportPathsAsync(paths, ownerWindow);
        }
        catch (Exception ex)
        {
            PrimalEditor_Avalonia.Common.Logger.Error("Dropped-file import failed", ex);
        }
    }

    private static bool HasDroppedFiles(Avalonia.Input.DragEventArgs e) =>
        e.DataTransfer.Contains(Avalonia.Input.DataFormat.File);

    private async void OnImportClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is ContentBrowserViewModel vm)
        {
            await vm.ImportAssetAsync();
        }
    }

    private async void OnNewFolderClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not ContentBrowserViewModel vm) return;
        if (vm.Project == null) return;
        var parent = vm.SelectedFolder != null && vm.SelectedFolder.FullPath != vm.Project.ContentPath
            ? vm.SelectedFolder.FullPath
            : vm.Project.ContentPath;
        var name = await PromptForTextAsync("New Folder", "Folder name:", "NewFolder");
        if (string.IsNullOrWhiteSpace(name)) return;
        try
        {
            var newPath = System.IO.Path.Combine(parent, name);
            if (System.IO.Directory.Exists(newPath))
            {
                PrimalEditor_Avalonia.Common.Logger.Warning($"NewFolder: '{newPath}' already exists");
                return;
            }
            System.IO.Directory.CreateDirectory(newPath);
            AssetRegistry.Register(newPath);
            vm.RefreshFolders();
        }
        catch (System.Exception ex)
        {
            PrimalEditor_Avalonia.Common.Logger.Error("NewFolder failed", ex);
        }
    }

    private async void OnRenameClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not ContentBrowserViewModel vm) return;
        if (vm.SelectedFile == null) return;
        var oldPath = vm.SelectedFile.FullPath;
        var oldName = System.IO.Path.GetFileName(oldPath);
        var newName = await PromptForTextAsync("Rename", "New name:", oldName);
        if (string.IsNullOrWhiteSpace(newName) || newName == oldName) return;
        try
        {
            var parent = System.IO.Path.GetDirectoryName(oldPath);
            if (string.IsNullOrEmpty(parent)) return;
            var newPath = System.IO.Path.Combine(parent, newName);
            if (System.IO.File.Exists(newPath) || System.IO.Directory.Exists(newPath))
            {
                PrimalEditor_Avalonia.Common.Logger.Warning($"Rename: '{newPath}' already exists");
                return;
            }
            // AssetRegistry.RenameAsset moves the file + sidecar in
            // lockstep and refreshes both the in-memory cache and the
            // SQLite index with the new relative path. The GUID
            // baked into the .meta survives.
            AssetRegistry.RenameAsset(oldPath, newPath);
            vm.RefreshFolders();
        }
        catch (System.Exception ex)
        {
            PrimalEditor_Avalonia.Common.Logger.Error("Rename failed", ex);
        }
    }

    private async void OnDeleteClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not ContentBrowserViewModel vm) return;
        if (vm.SelectedFile == null) return;
        var path = vm.SelectedFile.FullPath;
        var name = System.IO.Path.GetFileName(path);
        var isDir = System.IO.Directory.Exists(path);

        // Pre-flight: list dependents so the user understands the
        // blast radius before pulling the trigger. Direct dependents
        // only (no recursion); we surface names so the dialog is
        // meaningful even with a large project.
        var dependents = AssetRegistry.ListDeletionImpact(path);
        string impact;
        if (dependents.Count == 0)
        {
            impact = "No other assets reference this one.";
        }
        else
        {
            var preview = string.Join(", ", dependents.Take(8).Select(d => d.FileName));
            var more = dependents.Count > 8 ? $", and {dependents.Count - 8} more" : string.Empty;
            impact = $"{dependents.Count} asset(s) reference this one and will need their GUIDs broken: {preview}{more}.";
        }

        var dlg = PrimalEditor_Avalonia.Views.ConfirmDialog.Create(
            "Delete asset",
            $"Are you sure you want to delete '{(isDir ? name + "/" : name)}'? This cannot be undone.\n\n{impact}");
        var top = TopLevel.GetTopLevel(this) as Window;
        if (top == null) return;
        var r = await dlg.ShowDialogAsync(top);
        if (r != ConfirmResult.Save && r != PrimalEditor_Avalonia.Views.ConfirmResult.Discard) return;
        try
        {
            if (isDir)
            {
                System.IO.Directory.Delete(path, recursive: true);
            }
            else
            {
                System.IO.File.Delete(path);
                var meta = path + ".meta";
                if (System.IO.File.Exists(meta)) System.IO.File.Delete(meta);
            }
            AssetRegistry.Unregister(path);
            vm.RefreshFolders();
        }
        catch (System.Exception ex)
        {
            PrimalEditor_Avalonia.Common.Logger.Error("Delete failed", ex);
        }
    }

    private Task<string?> PromptForTextAsync(string title, string prompt, string initial)
    {
        var tcs = new System.Threading.Tasks.TaskCompletionSource<string?>();
        var dlg = new PrimalEditor_Avalonia.Views.TextInputDialog(title, prompt, initial);
        var top = TopLevel.GetTopLevel(this) as Window;
        if (top == null) { tcs.SetResult(null); return tcs.Task; }
        dlg.ShowDialogAsync(top).ContinueWith(t => tcs.SetResult(t.Result),
            System.Threading.Tasks.TaskScheduler.FromCurrentSynchronizationContext());
        return tcs.Task;
    }
}
