using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
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
        var topLevel = Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            ? desktop.MainWindow : null;

        if (topLevel == null) return;

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

    public void RemoveSelected()
    {
        if (SelectedProject != null)
        {
            _recentManager.RecentProjects.Remove(SelectedProject.Path);
            _recentManager.Save();
            RefreshRecentProjects();
            SelectedProject = null;
        }
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