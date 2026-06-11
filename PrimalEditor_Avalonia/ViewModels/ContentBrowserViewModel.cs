using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
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

public class ContentFile : ViewModelBase
{
    public string Name { get; set; } = string.Empty;
    public string FullPath { get; set; } = string.Empty;
    public long Size { get; set; }
    public AssetType Type { get; set; } = AssetType.Unknown;
    public bool IsImage =>
        Type == AssetType.Texture &&
        (Name.EndsWith(".png", StringComparison.OrdinalIgnoreCase) ||
         Name.EndsWith(".jpg", StringComparison.OrdinalIgnoreCase) ||
         Name.EndsWith(".jpeg", StringComparison.OrdinalIgnoreCase) ||
         Name.EndsWith(".bmp", StringComparison.OrdinalIgnoreCase));
    public string SizeDisplay => Size switch
    {
        < 1024 => $"{Size} B",
        < 1024 * 1024 => $"{Size / 1024.0:F1} KB",
        _ => $"{Size / (1024.0 * 1024.0):F1} MB",
    };
}

public class ContentBrowserViewModel : ViewModelBase
{
    private Project? _project;
    private ContentFolder? _selectedFolder;
    private ContentFile? _selectedFile;
    private string _statusMessage = string.Empty;
    private bool _isImporting;
    private string _importingStatus = string.Empty;
    private FileViewMode _viewMode = FileViewMode.List;

    public ObservableCollection<ContentFolder> Folders { get; } = new();
    public ObservableCollection<ContentFile> Files { get; } = new();

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
            }
        }
    }

    public ContentFile? SelectedFile
    {
        get => _selectedFile;
        set
        {
            if (_selectedFile != value)
            {
                _selectedFile = value;
                OnPropertyChanged(nameof(SelectedFile));
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

    public void RefreshFolders()
    {
        var previouslySelected = _selectedFolder?.FullPath;
        Folders.Clear();
        if (_project == null || !Directory.Exists(_project.ContentPath))
        {
            StatusMessage = "No project loaded";
            return;
        }

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
            return;
        }

        try
        {
            foreach (var file in Directory.GetFiles(SelectedFolder.FullPath))
            {
                var name = Path.GetFileName(file);
                if (name.StartsWith('.')) continue;

                var info = new FileInfo(file);
                Files.Add(new ContentFile
                {
                    Name = name,
                    FullPath = info.FullName,
                    Size = info.Length,
                    Type = AssetImporter.DetectType(name),
                });
            }
            StatusMessage = $"{Files.Count} file(s)";
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

        var subfolder = SelectedFolder != null && SelectedFolder.FullPath != _project.ContentPath
            ? Path.GetFileName(SelectedFolder.FullPath)
            : null;

        var imported = 0;
        var failed = 0;
        var firstError = string.Empty;
        var total = files.Count;
        var index = 0;

        IsImporting = true;
        try
        {
            foreach (var file in files)
            {
                index++;
                var name = Path.GetFileName(file.Path.LocalPath);
                var type = AssetImporter.DetectType(name);
                if (type == AssetType.Mesh)
                {
                    ImportingStatus = $"Cooking {name} → {Path.ChangeExtension(name, ".mesh")} ({index}/{total}) — large meshes may take several minutes...";
                }
                else
                {
                    ImportingStatus = $"Importing {name} ({index}/{total})...";
                }

                var asset = await AssetImporter.ImportAsync(file.Path.LocalPath, _project.ContentPath, subfolder);
                if (asset != null && asset.Success)
                {
                    imported++;
                    Logger.Info($"Imported: {Path.GetFileName(asset.DestinationPath)} ({asset.Type})");
                }
                else
                {
                    failed++;
                    if (asset?.ErrorMessage != null)
                    {
                        Logger.Warning($"Import failed: {name} - {asset.ErrorMessage}");
                        if (string.IsNullOrEmpty(firstError))
                        {
                            firstError = $"{name}: {asset.ErrorMessage}";
                        }
                    }
                    else
                    {
                        Logger.Warning($"Import failed: {name}");
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
            IsImporting = false;
            ImportingStatus = string.Empty;
        }

        StatusMessage = failed > 0 && !string.IsNullOrEmpty(firstError)
            ? $"Imported: {imported}, Failed: {failed}. First error: {firstError}"
            : $"Imported: {imported}, Failed: {failed}";
        RefreshFolders();
    }
}