using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.ViewModels;

public class RecentProjectEntry : ViewModelBase
{
    private string _name = string.Empty;
    private string _path = string.Empty;

    public string Name
    {
        get => _name;
        set
        {
            if (_name != value)
            {
                _name = value;
                OnPropertyChanged(nameof(Name));
            }
        }
    }

    public string Path
    {
        get => _path;
        set
        {
            if (_path != value)
            {
                _path = value;
                OnPropertyChanged(nameof(Path));
            }
        }
    }
}

public class OpenProjectViewModel : ViewModelBase
{
    private readonly RecentProjectsManager _recentManager = new();
    private RecentProjectEntry? _selectedProject;
    private string _errorMessage = string.Empty;

    public ObservableCollection<RecentProjectEntry> RecentProjects { get; } = new();

    public RecentProjectEntry? SelectedProject
    {
        get => _selectedProject;
        set
        {
            if (_selectedProject != value)
            {
                _selectedProject = value;
                OnPropertyChanged(nameof(SelectedProject));
                OnPropertyChanged(nameof(HasSelection));
            }
        }
    }

    public bool HasSelection => _selectedProject != null;

    public string ErrorMessage
    {
        get => _errorMessage;
        set
        {
            if (_errorMessage != value)
            {
                _errorMessage = value;
                OnPropertyChanged(nameof(ErrorMessage));
                OnPropertyChanged(nameof(HasError));
            }
        }
    }

    public bool HasError => !string.IsNullOrEmpty(_errorMessage);

    public OpenProjectViewModel()
    {
        _recentManager.Load();
        RefreshRecentProjects();
    }

    private void RefreshRecentProjects()
    {
        RecentProjects.Clear();
        foreach (var path in _recentManager.RecentProjects)
        {
            RecentProjects.Add(new RecentProjectEntry
            {
                Name = Path.GetFileNameWithoutExtension(path),
                Path = path,
            });
        }
    }

    public async void BrowseForFile()
    {
        await BrowseForFileAsync(null);
    }

    internal static TopLevel? ResolveFallbackTopLevel()
    {
        if (Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop)
        {
            return desktop.MainWindow
                ?? desktop.Windows.FirstOrDefault(w => w.IsVisible);
        }

        return null;
    }

    /// <summary>
    /// Show the project file picker. The owner must be passed in by the
    /// view: the project browser runs before the app has a MainWindow, so
    /// resolving the top level from the application lifetime yields null
    /// and the picker never opens.
    /// </summary>
    public async Task BrowseForFileAsync(TopLevel? owner)
    {
        var topLevel = owner ?? ResolveFallbackTopLevel();
        if (topLevel == null)
        {
            ErrorMessage = "Unable to open the file picker.";
            return;
        }

        var options = new FilePickerOpenOptions
        {
            Title = "Open Project",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("Primal Project")
                {
                    Patterns = new[] { "*.primal" },
                },
            },
        };

        var result = await topLevel.StorageProvider.OpenFilePickerAsync(options);
        if (result != null && result.Count > 0)
        {
            var path = result[0].Path.LocalPath;
            _recentManager.Add(path);
            _recentManager.Save();
            RefreshRecentProjects();
            SelectedProject = RecentProjects.FirstOrDefault(p => p.Path == path);
        }
    }

    /// <summary>
    /// Drop the selected project from the recent list and delete its folder
    /// from disk. The caller is responsible for confirming with the user
    /// first; this method does not prompt.
    /// </summary>
    public void RemoveSelected()
    {
        if (SelectedProject == null)
        {
            return;
        }

        var path = SelectedProject.Path;
        ErrorMessage = string.Empty;

        try
        {
            DeleteProject.DeleteProjectFolder(path);
        }
        catch (System.Exception ex)
        {
            // The recent-list entry stays put so the user can retry or
            // clean up by hand; removing it would hide a project that is
            // still on disk.
            ErrorMessage = $"Could not delete project files: {ex.Message}";
            return;
        }

        _recentManager.RecentProjects.Remove(path);
        _recentManager.Save();
        RefreshRecentProjects();
        SelectedProject = null;
    }

    public string? GetSelectedProjectPath()
    {
        ErrorMessage = string.Empty;
        if (SelectedProject == null)
        {
            ErrorMessage = "Please select a project";
            return null;
        }

        return SelectedProject.Path;
    }

    public Project? OpenSelectedProject()
    {
        ErrorMessage = string.Empty;

        if (SelectedProject == null)
        {
            ErrorMessage = "Please select a project";
            return null;
        }

        try
        {
            var project = GameProject.OpenProject.Open(SelectedProject.Path);
            if (project != null)
            {
                _recentManager.Add(SelectedProject.Path);
                _recentManager.Save();
                Logger.Info($"Project opened via dialog: {project.FullPath}");
            }
            else
            {
                ErrorMessage = "Failed to open project file";
                _ = Common.ErrorDialog.ShowAsync(
                    "Cannot Open Project",
                    $"The project file is corrupted or invalid:\n{SelectedProject.Path}\n\nCheck the Logger for details.",
                    Common.ErrorSeverity.Error);
            }
            return project;
        }
        catch (System.Exception ex)
        {
            ErrorMessage = $"Error: {ex.Message}";
            Logger.Error("Failed to open project", ex);
            _ = Common.ErrorDialog.ShowAsync(
                "Cannot Open Project",
                $"An error occurred while opening the project:\n{ex.Message}",
                Common.ErrorSeverity.Error);
            return null;
        }
    }
}
