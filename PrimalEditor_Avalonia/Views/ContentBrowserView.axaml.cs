using System.Linq;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
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
        if (DataContext is ContentBrowserViewModel vm)
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
}