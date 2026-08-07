using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Dock.Model.Mvvm.Controls;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Import;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.Importers;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.ViewModels;

public class ContentFolder : ViewModelBase
{
    public string Name { get; set; } = string.Empty;
    public string FullPath { get; set; } = string.Empty;
    public ObservableCollection<ContentFolder> Subfolders { get; } = new();
}

public enum FileViewMode
{
    List,
    Thumbnails,
}

public class ContentBrowserViewModel : Document
{
    private Project? _project;
    private ContentFolder? _selectedFolder;
    private AssetInfo? _selectedFile;
    private string _statusMessage = string.Empty;
    private bool _isImporting;
    private string _importingStatus = string.Empty;
    private FileViewMode _viewMode = FileViewMode.Thumbnails;
    private string _searchQuery = string.Empty;

    public ObservableCollection<ContentFolder> Folders { get; } = new();
    public ObservableCollection<AssetInfo> Files { get; } = new();
    public ObservableCollection<AssetInfo> FilteredFiles { get; } = new();
    public ObservableCollection<BreadcrumbSegment> Breadcrumbs { get; } = new();

    public ContentBrowserViewModel()
    {
        ContentWatcher.ContentModified += OnContentModified;
    }

    private void OnContentModified(object? sender, ContentModifiedEventArgs e)
    {
        // The debouncer emits a single "All" event when its timer fires;
        // every other invocation carries an OldPath or NewPath that we
        // can apply directly. Re-register the affected path on the
        // AssetRegistry so the GUI sees the new/removed asset.
        switch (e.ChangeType)
        {
            case System.IO.WatcherChangeTypes.Created when !string.IsNullOrEmpty(e.NewPath):
                if (!IsMetaSidecarPath(e.NewPath!))
                    PrimalEditor_Avalonia.Content.AssetRegistry.Register(e.NewPath!);
                break;
            case System.IO.WatcherChangeTypes.Deleted when !string.IsNullOrEmpty(e.OldPath):
                PrimalEditor_Avalonia.Content.AssetRegistry.Unregister(e.NewPath ?? e.OldPath!);
                break;
            case System.IO.WatcherChangeTypes.Renamed when !string.IsNullOrEmpty(e.OldPath) && !string.IsNullOrEmpty(e.NewPath):
                PrimalEditor_Avalonia.Content.AssetRegistry.Unregister(e.OldPath!);
                if (!IsMetaSidecarPath(e.NewPath!))
                    PrimalEditor_Avalonia.Content.AssetRegistry.Register(e.NewPath!);
                break;
            case System.IO.WatcherChangeTypes.All:
                // Debounced flush — refresh the whole tree.
                Avalonia.Threading.Dispatcher.UIThread.Post(RefreshFolders);
                return;
        }
        Avalonia.Threading.Dispatcher.UIThread.Post(RefreshFolders);
    }

    private static bool IsMetaSidecarPath(string path)
    {
        var name = System.IO.Path.GetFileName(path);
        return name.EndsWith(".meta", System.StringComparison.OrdinalIgnoreCase);
    }

    public Project? Project
    {
        get => _project;
        set
        {
            if (_project != value)
            {
                _project = value;
                OnPropertyChanged(nameof(Project));
                RefreshFolders();
                RebuildBreadcrumbs();
            }
        }
    }

    public ContentFolder? SelectedFolder
    {
        get => _selectedFolder;
        set
        {
            if (_selectedFolder != value)
            {
                _selectedFolder = value;
                OnPropertyChanged(nameof(SelectedFolder));
                LoadFilesForSelectedFolder();
                RebuildBreadcrumbs();
            }
        }
    }

    public AssetInfo? SelectedFile
    {
        get => _selectedFile;
        set
        {
            if (_selectedFile != value)
            {
                _selectedFile = value;
                OnPropertyChanged(nameof(SelectedFile));
                PreviewViewModel.Asset = value;
            }
        }
    }

    public string StatusMessage
    {
        get => _statusMessage;
        set
        {
            if (_statusMessage != value)
            {
                _statusMessage = value;
                OnPropertyChanged(nameof(StatusMessage));
                OnPropertyChanged(nameof(HasStatus));
            }
        }
    }

    public bool HasStatus => !string.IsNullOrEmpty(_statusMessage);

    public bool IsImporting
    {
        get => _isImporting;
        set
        {
            if (_isImporting != value)
            {
                _isImporting = value;
                OnPropertyChanged(nameof(IsImporting));
            }
        }
    }

    public string ImportingStatus
    {
        get => _importingStatus;
        set
        {
            if (_importingStatus != value)
            {
                _importingStatus = value;
                OnPropertyChanged(nameof(ImportingStatus));
            }
        }
    }

    public FileViewMode ViewMode
    {
        get => _viewMode;
        set
        {
            if (_viewMode != value)
            {
                _viewMode = value;
                OnPropertyChanged(nameof(ViewMode));
            }
        }
    }

    /// <summary>
    /// Side-pane preview VM bound by ContentBrowserView's third column.
    /// Owned here so its lifetime matches the browser and so the same
    /// instance can be reused across selection changes.
    /// </summary>
    public ContentBrowserPreviewViewModel PreviewViewModel { get; } = new();

    public string SearchQuery
    {
        get => _searchQuery;
        set
        {
            if (_searchQuery != value)
            {
                _searchQuery = value ?? string.Empty;
                OnPropertyChanged(nameof(SearchQuery));
                OnPropertyChanged(nameof(HasActiveSearch));
                RebuildFilteredFiles();
                OnPropertyChanged(nameof(ResultCountDisplay));
            }
        }
    }

    public bool HasActiveSearch => !string.IsNullOrEmpty(_searchQuery);

    public string ResultCountDisplay =>
        HasActiveSearch
            ? $"{FilteredFiles.Count} of {Files.Count}"
            : string.Empty;

    public void ClearSearch()
    {
        SearchQuery = string.Empty;
    }

    public void RefreshFolders()
    {
        var previouslySelected = _selectedFolder?.FullPath;
        Folders.Clear();
        if (_project == null || !Directory.Exists(_project.ContentPath))
        {
            AssetRegistry.Reset();
            StatusMessage = "No project loaded";
            return;
        }

        AssetRegistry.Initialize(_project.ContentPath);

        try
        {
            var root = new ContentFolder
            {
                Name = "Content",
                FullPath = _project.ContentPath,
            };
            LoadSubfolders(root);
            Folders.Add(root);
            StatusMessage = string.Empty;

            ContentFolder? match = null;
            if (!string.IsNullOrEmpty(previouslySelected))
            {
                match = FindFolderByPath(Folders, previouslySelected);
            }
            if (match == null)
            {
                match = Folders.FirstOrDefault();
            }
            if (match != null)
            {
                _selectedFolder = match;
                OnPropertyChanged(nameof(SelectedFolder));
                LoadFilesForSelectedFolder();
            }
        }
        catch (UnauthorizedAccessException ex)
        {
            StatusMessage = $"Permission denied: {ex.Message}";
            Logger.Error("Permission denied accessing content folder", ex);
        }
        catch (DirectoryNotFoundException ex)
        {
            StatusMessage = "Content folder not found";
            Logger.Error("Content folder not found", ex);
        }
        catch (Exception ex)
        {
            StatusMessage = $"Error: {ex.Message}";
            Logger.Error("Error loading content folders", ex);
        }
    }

    private static ContentFolder? FindFolderByPath(ObservableCollection<ContentFolder> folders, string fullPath)
    {
        foreach (var f in folders)
        {
            if (f.FullPath == fullPath) return f;
            var nested = FindFolderByPath(f.Subfolders, fullPath);
            if (nested != null) return nested;
        }
        return null;
    }

    private void RebuildFilteredFiles()
    {
        FilteredFiles.Clear();
        foreach (var file in Files)
        {
            if (AssetSearchFilter.Match(file.FileName, _searchQuery))
                FilteredFiles.Add(file);
        }
    }

    private void RebuildBreadcrumbs()
    {
        Breadcrumbs.Clear();
        var root = _project?.ContentPath;
        if (string.IsNullOrEmpty(root))
        {
            OnPropertyChanged(nameof(HasBreadcrumbs));
            return;
        }
        var selected = _selectedFolder?.FullPath;
        foreach (var seg in BreadcrumbBuilder.Build(root, selected))
            Breadcrumbs.Add(seg);
        OnPropertyChanged(nameof(HasBreadcrumbs));
    }

    public bool HasBreadcrumbs => Breadcrumbs.Count > 0;

    public void NavigateToBreadcrumb(BreadcrumbSegment segment)
    {
        if (segment == null || _project == null) return;
        if (segment.IsRoot)
        {
            var root = Folders.FirstOrDefault();
            if (root != null) SelectedFolder = root;
            return;
        }
        var match = FindFolderByPath(Folders, segment.FullPath);
        if (match != null) SelectedFolder = match;
    }

    private void LoadSubfolders(ContentFolder folder)
    {
        try
        {
            foreach (var dir in Directory.GetDirectories(folder.FullPath))
            {
                var subfolder = new ContentFolder
                {
                    Name = Path.GetFileName(dir),
                    FullPath = dir,
                };
                LoadSubfolders(subfolder);
                folder.Subfolders.Add(subfolder);
            }
        }
        catch (UnauthorizedAccessException) { }
        catch (DirectoryNotFoundException) { }
    }

    public void LoadFilesForSelectedFolder()
    {
        Files.Clear();
        if (SelectedFolder == null || !Directory.Exists(SelectedFolder.FullPath))
        {
            RebuildFilteredFiles();
            OnPropertyChanged(nameof(ResultCountDisplay));
            return;
        }

        try
        {
            AssetRegistry.Register(SelectedFolder.FullPath);
            foreach (var dir in Directory.GetDirectories(SelectedFolder.FullPath))
                AssetRegistry.Register(dir);
            foreach (var file in Directory.GetFiles(SelectedFolder.FullPath))
            {
                var name = Path.GetFileName(file);
                if (name.StartsWith('.')) continue;

                var info = AssetRegistry.Register(file);
                if (info != null && info.Type != AssetType.Unknown)
                    Files.Add(info);
            }
            StatusMessage = $"{Files.Count} asset(s)";
            RebuildFilteredFiles();
            OnPropertyChanged(nameof(ResultCountDisplay));
        }
        catch (UnauthorizedAccessException ex)
        {
            StatusMessage = $"Permission denied: {ex.Message}";
            Logger.Error("Permission denied loading files", ex);
        }
        catch (Exception ex)
        {
            StatusMessage = $"Error: {ex.Message}";
            Logger.Error("Error loading files", ex);
        }
    }

    public void OpenSelectedFile()
    {
        if (SelectedFile == null) return;
        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = SelectedFile.FullPath,
                UseShellExecute = true,
            });
            Logger.Info($"Opened file: {SelectedFile.FullPath}");
        }
        catch (Exception ex)
        {
            StatusMessage = $"Cannot open file: {ex.Message}";
            Logger.Error("Failed to open file", ex);
        }
    }

    public async Task ImportAssetAsync()
    {
        if (_project == null)
        {
            Logger.Warning("Cannot import asset: no project loaded");
            return;
        }

        var topLevel = Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            ? desktop.MainWindow : null;
        if (topLevel == null) return;

        var options = new Avalonia.Platform.Storage.FilePickerOpenOptions
        {
            Title = "Import Asset",
            AllowMultiple = true,
            FileTypeFilter = new[]
            {
                new Avalonia.Platform.Storage.FilePickerFileType("All supported")
                {
                    Patterns = new[] { "*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp", "*.dds", "*.hdr", "*.fbx", "*.obj", "*.gltf", "*.glb", "*.wav", "*.mp3", "*.ogg" }
                },
                new Avalonia.Platform.Storage.FilePickerFileType("Textures")
                {
                    Patterns = new[] { "*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp", "*.dds", "*.hdr", "*.exr", "*.ktx", "*.ktx2" }
                },
                new Avalonia.Platform.Storage.FilePickerFileType("Models")
                {
                    Patterns = new[] { "*.fbx", "*.obj", "*.gltf", "*.glb", "*.dae", "*.blend" }
                },
                new Avalonia.Platform.Storage.FilePickerFileType("Audio")
                {
                    Patterns = new[] { "*.wav", "*.mp3", "*.ogg", "*.flac" }
                },
                new Avalonia.Platform.Storage.FilePickerFileType("All files")
                {
                    Patterns = new[] { "*" }
                },
            },
        };

        var files = await topLevel.StorageProvider.OpenFilePickerAsync(options);
        if (files == null || files.Count == 0) return;

        await ImportPathsAsync(files.Select(f => f.Path.LocalPath).ToList(), topLevel);
    }

    /// <summary>
    /// Drive the per-category dialog flow + plugin dispatch for an arbitrary
    /// set of file paths. Used by both the Import button (with picker paths)
    /// and the drag-and-drop handler (with dropped paths). Caller supplies
    /// the owner window for modal dialogs.
    /// </summary>
    public async Task ImportPathsAsync(IReadOnlyList<string> sourcePaths, Avalonia.Controls.Window owner)
    {
        if (_project == null)
        {
            Logger.Warning("Cannot import asset: no project loaded");
            return;
        }
        if (sourcePaths == null || sourcePaths.Count == 0) return;

        var subfolder = SelectedFolder != null && SelectedFolder.FullPath != _project.ContentPath
            ? Path.GetFileName(SelectedFolder.FullPath)
            : null;

        var paths = sourcePaths.ToList();
        var settingsPerFile = new Dictionary<string, IAssetImportSettings>();

        foreach (var group in paths.GroupBy(p => AssetImporterRegistry.ResolveForFile(p)?.GetType() ?? typeof(object))
                     .Where(g => g.Key != typeof(object)))
        {
            var firstPath = group.First();
            var plugin = AssetImporterRegistry.ResolveForFile(firstPath);
            if (plugin == null) continue;

            var defaults = ImportSettingsDefaults.ResolveDefaults(plugin, _project, group.ToList());
            var (accepted, settings) = await ImportSettingsDialog.ShowAsync(owner, plugin, group.ToList(), defaults);
            if (!accepted || settings == null) continue;
            foreach (var p in group)
                settingsPerFile[p] = settings;
        }

        if (settingsPerFile.Count == 0)
        {
            RefreshFolders();
            return;
        }

        var destDir = string.IsNullOrEmpty(subfolder) ? _project.ContentPath : Path.Combine(_project.ContentPath, subfolder);
        var imported = 0;
        var failed = 0;
        var firstError = string.Empty;
        var total = settingsPerFile.Count;
        var index = 0;

        IsImporting = true;
        try
        {
            foreach (var (sourcePath, settings) in settingsPerFile)
            {
                index++;
                var name = Path.GetFileName(sourcePath);
                var plugin = AssetImporterRegistry.ResolveForFile(sourcePath);
                if (plugin == null) continue;
                if (plugin is MeshImporterPlugin)
                {
                    ImportingStatus = $"Cooking {name} → {Path.ChangeExtension(name, ".mesh")} ({index}/{total}) — large meshes may take several minutes...";
                }
                else
                {
                    ImportingStatus = $"Importing {name} ({index}/{total})...";
                }

                var asset = await plugin.RunAsync(new ImportRequest
                {
                    SourcePath = sourcePath,
                    DestinationDir = destDir,
                    Subfolder = subfolder,
                    Settings = settings,
                }, progress: null, CancellationToken.None);

                if (asset != null && asset.Success)
                {
                    imported++;
                    AssetRegistry.Register(asset.DestinationPath);
                    Logger.Info($"Imported: {Path.GetFileName(asset.DestinationPath)} ({asset.Type})");
                }
                else
                {
                    failed++;
                    var msg = asset?.ErrorMessage ?? "unknown error";
                    Logger.Warning($"Import failed: {name} - {msg}");
                    if (string.IsNullOrEmpty(firstError)) firstError = $"{name}: {msg}";
                }
            }
        }
        finally
        {
            IsImporting = false;
            ImportingStatus = string.Empty;
        }

        StatusMessage = failed > 0
            ? $"Imported: {imported}, Failed: {failed}. First error: {firstError}"
            : $"Imported: {imported}, Failed: {failed}";
        RefreshFolders();
    }
}