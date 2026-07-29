using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class ContentBrowserView : UserControl
{
    public ContentBrowserView()
    {
        InitializeComponent();
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private void OnFolderDoubleClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is ContentBrowserViewModel vm)
        {
            vm.LoadFilesForSelectedFolder();
        }
    }

    private void OnFileDoubleClick(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        if (DataContext is not ContentBrowserViewModel vm) return;
        if (vm.SelectedFile is AssetInfo info)
        {
            if (AssetEditorRegistry.Open(info) == null)
                vm.OpenSelectedFile();
        }
        else
        {
            vm.OpenSelectedFile();
        }
    }

    private void OnDragEnter(object? sender, Avalonia.Input.DragEventArgs e)
    {
        if (!e.Data.Contains(Avalonia.Input.DataFormats.Files)) return;
        e.DragEffects = Avalonia.Input.DragDropEffects.Copy;
        DropOverlay.IsVisible = true;
    }

    private void OnDragLeave(object? sender, Avalonia.Input.DragEventArgs e)
    {
        DropOverlay.IsVisible = false;
    }

    private void OnDragOver(object? sender, Avalonia.Input.DragEventArgs e)
    {
        if (!e.Data.Contains(Avalonia.Input.DataFormats.Files)) return;
        e.DragEffects = Avalonia.Input.DragDropEffects.Copy;
    }

    private async void OnFileDrop(object? sender, Avalonia.Input.DragEventArgs e)
    {
        DropOverlay.IsVisible = false;

        if (DataContext is not ContentBrowserViewModel vm) return;
        if (vm.Project == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning("Cannot drop files: no project loaded");
            return;
        }

        var files = e.Data.Contains(Avalonia.Input.DataFormats.Files)
            ? e.Data.Get(Avalonia.Input.DataFormats.Files) as System.Collections.IEnumerable
            : null;

        var paths = new System.Collections.Generic.List<string>();
        if (files != null)
        {
            foreach (var f in files)
            {
                var p = f as string;
                if (!string.IsNullOrEmpty(p)) paths.Add(p);
                else if (f is Avalonia.Platform.Storage.IStorageItem item && item.Path.IsFile)
                    paths.Add(item.Path.LocalPath);
            }
        }

        if (paths.Count == 0) return;

        var subfolder = vm.SelectedFolder != null && vm.SelectedFolder.FullPath != vm.Project.ContentPath
            ? System.IO.Path.GetFileName(vm.SelectedFolder.FullPath)
            : null;

        var imported = 0;
        var failed = 0;
        var firstError = string.Empty;
        var total = paths.Count;
        var index = 0;

        vm.IsImporting = true;
        try
        {
            foreach (var path in paths)
            {
                index++;
                var name = System.IO.Path.GetFileName(path);
                var type = Native.AssetImporter.DetectType(name);
                if (type == Native.AssetType.Mesh)
                {
                    vm.ImportingStatus = $"Cooking {name} → {System.IO.Path.ChangeExtension(name, ".mesh")} ({index}/{total}) — large meshes may take several minutes...";
                }
                else
                {
                    vm.ImportingStatus = $"Importing {name} ({index}/{total})...";
                }

                var asset = await Native.AssetImporter.ImportAsync(path, vm.Project.ContentPath, subfolder);
                if (asset != null && asset.Success)
                {
                    imported++;
                    PrimalEditor_Avalonia.Common.Logger.Info($"Imported (drop): {name}");
                }
                else
                {
                    failed++;
                    if (asset?.ErrorMessage != null)
                    {
                        PrimalEditor_Avalonia.Common.Logger.Warning($"Drop import failed: {name} - {asset.ErrorMessage}");
                        if (string.IsNullOrEmpty(firstError))
                        {
                            firstError = $"{name}: {asset.ErrorMessage}";
                        }
                    }
                    else
                    {
                        PrimalEditor_Avalonia.Common.Logger.Warning($"Drop import failed: {name}");
                        if (string.IsNullOrEmpty(firstError))
                        {
                            firstError = $"{name}: unknown error (engine returned no result)";
                        }
                    }
                }
            }
        }
        finally
        {
            vm.IsImporting = false;
            vm.ImportingStatus = string.Empty;
        }

        vm.StatusMessage = failed > 0 && !string.IsNullOrEmpty(firstError)
            ? $"Imported: {imported}, Failed: {failed}. First error: {firstError}"
            : $"Imported: {imported}, Failed: {failed}";
        vm.RefreshFolders();
    }

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
            if (System.IO.File.Exists(oldPath))
            {
                System.IO.File.Move(oldPath, newPath);
                // Move the sidecar too if it exists.
                var oldMeta = oldPath + ".meta";
                if (System.IO.File.Exists(oldMeta))
                {
                    System.IO.File.Move(oldMeta, newPath + ".meta");
                }
            }
            else
            {
                System.IO.Directory.Move(oldPath, newPath);
            }
            AssetRegistry.Unregister(oldPath);
            AssetRegistry.Register(newPath);
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
        var dlg = PrimalEditor_Avalonia.Views.ConfirmDialog.Create(
            "Delete asset",
            $"Are you sure you want to delete '{(isDir ? name + "/" : name)}'? This cannot be undone.");
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