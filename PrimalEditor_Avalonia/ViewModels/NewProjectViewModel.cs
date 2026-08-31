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

public class NewProjectViewModel : ViewModelBase
{
    private string _projectName = "NewProject";
    private string _projectPath = string.Empty;
    private ProjectTemplate? _selectedTemplate;
    private string _errorMessage = string.Empty;

    public ObservableCollection<ProjectTemplate> Templates { get; } = new();

    public string ProjectName
    {
        get => _projectName;
        set
        {
            if (_projectName != value)
            {
                _projectName = value;
                OnPropertyChanged(nameof(ProjectName));
                OnPropertyChanged(nameof(CanCreate));
            }
        }
    }

    public string ProjectPath
    {
        get => _projectPath;
        set
        {
            if (_projectPath != value)
            {
                _projectPath = value;
                OnPropertyChanged(nameof(ProjectPath));
                OnPropertyChanged(nameof(CanCreate));
            }
        }
    }

    public ProjectTemplate? SelectedTemplate
    {
        get => _selectedTemplate;
        set
        {
            if (_selectedTemplate != value)
            {
                _selectedTemplate = value;
                OnPropertyChanged(nameof(SelectedTemplate));
                OnPropertyChanged(nameof(CanCreate));
            }
        }
    }

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

    public bool CanCreate =>
        !string.IsNullOrWhiteSpace(_projectName) &&
        !string.IsNullOrWhiteSpace(_projectPath) &&
        _selectedTemplate != null;

    public NewProjectViewModel()
    {
        foreach (var template in ProjectTemplates.GetDefaultTemplates())
        {
            Templates.Add(template);
        }
        SelectedTemplate = Templates.FirstOrDefault();
        ProjectPath = Path.Combine(
            System.Environment.GetFolderPath(System.Environment.SpecialFolder.UserProfile),
            "PrimalProjects");
    }

    public async void BrowseForPath()
    {
        await BrowseForPathAsync(null);
    }

    /// <summary>
    /// Show the folder picker. The owner must be passed in by the view:
    /// the project browser runs before the app has a MainWindow, so
    /// resolving the top level from the application lifetime yields null
    /// and the picker never opens.
    /// </summary>
    public async Task BrowseForPathAsync(TopLevel? owner)
    {
        var topLevel = owner ?? OpenProjectViewModel.ResolveFallbackTopLevel();
        if (topLevel == null)
        {
            ErrorMessage = "Unable to open the folder picker.";
            return;
        }

        var options = new FolderPickerOpenOptions
        {
            Title = "Select Project Location",
        };

        if (Directory.Exists(ProjectPath))
        {
            try
            {
                var folder = await topLevel.StorageProvider.TryGetFolderFromPathAsync(
                    new System.Uri(ProjectPath));
                if (folder != null)
                {
                    options.SuggestedStartLocation = folder;
                }
            }
            catch { }
        }

        var result = await topLevel.StorageProvider.OpenFolderPickerAsync(options);
        if (result != null && result.Count > 0)
        {
            var path = result[0].Path.LocalPath;
            if (!string.IsNullOrEmpty(path))
            {
                ProjectPath = path;
            }
        }
    }

    public Project? CreateProject()
    {
        ErrorMessage = string.Empty;

        if (string.IsNullOrWhiteSpace(ProjectName))
        {
            ErrorMessage = "Project name cannot be empty";
            return null;
        }

        if (string.IsNullOrWhiteSpace(ProjectPath))
        {
            ErrorMessage = "Project path cannot be empty";
            return null;
        }

        if (SelectedTemplate == null)
        {
            ErrorMessage = "Please select a template";
            return null;
        }

        try
        {
            var project = NewProject.Create(ProjectName, ProjectPath, SelectedTemplate);
            Logger.Info($"Project created via dialog: {project.FullPath}");
            return project;
        }
        catch (System.Exception ex)
        {
            ErrorMessage = $"Failed to create project: {ex.Message}";
            Logger.Error("Project creation failed", ex);
            return null;
        }
    }
}