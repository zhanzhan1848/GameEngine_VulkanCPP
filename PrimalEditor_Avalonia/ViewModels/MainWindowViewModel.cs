using System.Collections.Generic;
using System.Windows.Input;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Views;

namespace PrimalEditor_Avalonia.ViewModels;

public class MainWindowViewModel : ViewModelBase
{
    private Project? _project;
    private string _windowTitle = "PrimalEditor Avalonia";
    private bool _isLoggerVisible = true;

    public WorldEditorViewModel WorldEditor { get; } = new WorldEditorViewModel();

    public ContentBrowserViewModel ContentBrowser { get; } = new ContentBrowserViewModel();

    public LoggerViewModel Logger { get; } = new LoggerViewModel();

    public bool IsLoggerVisible
    {
        get => _isLoggerVisible;
        set
        {
            if (_isLoggerVisible != value)
            {
                _isLoggerVisible = value;
                OnPropertyChanged(nameof(IsLoggerVisible));
            }
        }
    }

    public void ToggleLogger()
    {
        IsLoggerVisible = !IsLoggerVisible;
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
                OnPropertyChanged(nameof(ProjectPathDisplay));
                OnPropertyChanged(nameof(SceneCountDisplay));
                OnPropertyChanged(nameof(EntityCountDisplay));
                WorldEditor.Project = value;
                ContentBrowser.Project = value;
                if (value != null)
                {
                    WindowTitle = $"{value.Name} - PrimalEditor Avalonia";
                    WorldEditor.ActiveScene = value.ActiveScene;
                    PrimalEditor_Avalonia.Content.ContentWatcher.Initialize(value.ContentPath);
                }
                else
                {
                    WindowTitle = "PrimalEditor Avalonia";
                    PrimalEditor_Avalonia.Content.ContentWatcher.Reset();
                }
                PrimalEditor_Avalonia.Common.AutoSaveService.Track(value);
            }
        }
    }

    public string WindowTitle
    {
        get => _windowTitle;
        set
        {
            if (_windowTitle != value)
            {
                _windowTitle = value;
                OnPropertyChanged(nameof(WindowTitle));
            }
        }
    }

    public string ProjectPathDisplay => _project?.FullPath ?? "No project loaded";
    public string SceneCountDisplay => _project != null ? $"{_project.Scenes.Count} scene(s)" : "0 scenes";
    public string EntityCountDisplay
    {
        get
        {
            var count = 0;
            if (_project != null)
            {
                foreach (var scene in _project.Scenes)
                {
                    count += scene.Entities.Count;
                }
            }
            return $"{count} entities";
        }
    }

    public ICommand NewProjectCommand { get; }
    public ICommand OpenProjectCommand { get; }
    public ICommand SaveProjectCommand { get; }
    public ICommand ExitCommand { get; }
    public ICommand UndoCommand { get; }
    public ICommand RedoCommand { get; }
    public ICommand AddEntityCommand { get; }
    public ICommand DeleteEntityCommand { get; }
    public ICommand RenameEntityCommand { get; }
    public ICommand DuplicateEntityCommand { get; }
    public ICommand CutCommand { get; }
    public ICommand CopyCommand { get; }
    public ICommand PasteCommand { get; }
    public ICommand AboutCommand { get; }

    private List<GameEntity> _clipboard = new();

    public MainWindowViewModel()
    {
        NewProjectCommand = new RelayCommand(_ => NewProjectExecute());
        OpenProjectCommand = new RelayCommand(_ => OpenProjectExecute());
        SaveProjectCommand = new RelayCommand(_ => SaveProjectExecute(), _ => _project != null);
        ExitCommand = new RelayCommand(_ => ExitExecute());
        UndoCommand = new RelayCommand(_ => WorldEditor.UndoRedo.Undo(), _ => WorldEditor.UndoRedo.UndoStack.Count > 0);
        RedoCommand = new RelayCommand(_ => WorldEditor.UndoRedo.Redo(), _ => WorldEditor.UndoRedo.RedoStack.Count > 0);
        AddEntityCommand = new RelayCommand(_ => AddEntityExecute());
        DeleteEntityCommand = new RelayCommand(_ => DeleteEntityExecute());
        RenameEntityCommand = new RelayCommand(_ => RenameEntityExecute());
        DuplicateEntityCommand = new RelayCommand(_ => DuplicateEntityExecute());
        CutCommand = new RelayCommand(_ => CutExecute(), _ => WorldEditor.SelectedEntities.Length > 0);
        CopyCommand = new RelayCommand(_ => CopyExecute(), _ => WorldEditor.SelectedEntities.Length > 0);
        PasteCommand = new RelayCommand(_ => PasteExecute(), _ => _clipboard.Count > 0 && WorldEditor.ActiveScene != null);
        AboutCommand = new RelayCommand(_ => AboutExecute());
    }

    public void CutExecute()
    {
        CopyExecute();
        DeleteEntityExecute();
        PrimalEditor_Avalonia.Common.Logger.Info($"Cut {_clipboard.Count} entity(ies)");
    }

    public void CopyExecute()
    {
        _clipboard.Clear();
        foreach (var entity in WorldEditor.SelectedEntities)
        {
            _clipboard.Add(entity);
        }
        PrimalEditor_Avalonia.Common.Logger.Info($"Copied {_clipboard.Count} entity(ies) to clipboard");
    }

    public void PasteExecute()
    {
        if (WorldEditor.ActiveScene == null || _clipboard.Count == 0) return;
        foreach (var source in _clipboard)
        {
            var clone = new Components.GameEntity(source.Name + "_Copy")
            {
                IsEnabled = source.IsEnabled,
                IsActive = source.IsActive,
            };
            foreach (var component in source.Components)
            {
                if (component is Components.Transform t)
                {
                    clone.Components.Add(new Components.Transform
                    {
                        PositionX = t.PositionX,
                        PositionY = t.PositionY,
                        PositionZ = t.PositionZ,
                        RotationX = t.RotationX,
                        RotationY = t.RotationY,
                        RotationZ = t.RotationZ,
                        ScaleX = t.ScaleX,
                        ScaleY = t.ScaleY,
                        ScaleZ = t.ScaleZ,
                    });
                }
            }
            var action = new Common.AddEntityAction(WorldEditor, clone);
            WorldEditor.UndoRedo.Add(action);
            action.Redo();
        }
        PrimalEditor_Avalonia.Common.Logger.Info($"Pasted {_clipboard.Count} entity(ies)");
    }

    public void AboutExecute()
    {
        var topLevel = Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            ? desktop.MainWindow : null;
        if (topLevel != null)
        {
            var dialog = new Views.AboutDialog();
            dialog.ShowDialog(topLevel);
        }
    }

    public async void NewProjectExecute()
    {
        var dialog = new NewProjectView
        {
            DataContext = new NewProjectViewModel(),
        };
        var window = GetMainWindow();
        if (window != null)
        {
            var result = await dialog.ShowDialog<Project?>(window);
            if (result != null)
            {
                Project = result;
            }
        }
    }

    public async void OpenProjectExecute()
    {
        var dialog = new OpenProjectView
        {
            DataContext = new OpenProjectViewModel(),
        };
        var window = GetMainWindow();
        if (window != null)
        {
            var result = await dialog.ShowDialog<Project?>(window);
            if (result != null)
            {
                Project = result;
            }
        }
    }

    public void SaveProjectExecute()
    {
        if (_project == null) return;
        try
        {
            Serializer.SaveProject(_project);
            if (_project.ActiveScene != null)
            {
                NewProject.SaveScene(_project.ActiveScene, _project);
            }
        }
        catch (System.Exception ex)
        {
            PrimalEditor_Avalonia.Common.Logger.Error("Save failed", ex);
        }
    }

    public void ExitExecute()
    {
        if (Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.Shutdown();
        }
    }

    public void AddEntityExecute()
    {
        if (WorldEditor.ActiveScene == null) return;
        var entity = new Components.GameEntity("NewEntity");
        entity.Components.Add(new Components.Transform());
        var action = new AddEntityAction(WorldEditor, entity);
        WorldEditor.UndoRedo.Add(action);
        action.Redo();
    }

    public void DeleteEntityExecute()
    {
        foreach (var entity in WorldEditor.SelectedEntities)
        {
            var action = new RemoveEntityAction(WorldEditor, entity);
            WorldEditor.UndoRedo.Add(action);
            action.Redo();
        }
    }

    public void RenameEntityExecute()
    {
        if (WorldEditor.SelectedEntities.Length != 1) return;
        var entity = WorldEditor.SelectedEntities[0];
        var dialog = new RenameEntityDialog(entity.Name);
        var window = GetMainWindow();
        if (window != null)
        {
            var result = dialog.ShowDialog<string?>(window);
            result.Wait();
            if (!string.IsNullOrEmpty(result.Result))
            {
                var action = new RenameEntityAction(entity, result.Result);
                WorldEditor.UndoRedo.Add(action);
                action.Redo();
            }
        }
    }

    public void DuplicateEntityExecute()
    {
        foreach (var entity in WorldEditor.SelectedEntities)
        {
            var action = new DuplicateEntityAction(WorldEditor, entity);
            WorldEditor.UndoRedo.Add(action);
            action.Redo();
        }
    }

    private Avalonia.Controls.Window? GetMainWindow()
    {
        return Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            ? desktop.MainWindow : null;
    }
}