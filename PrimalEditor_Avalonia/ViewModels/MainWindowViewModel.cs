using System.Collections.Generic;
using System.Windows.Input;
using Dock.Model.Controls;
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
    private bool _isWorldEditorVisible = true;
    private bool _isContentBrowserVisible = true;

    public WorldEditorViewModel WorldEditor { get; } = new WorldEditorViewModel();

    public ContentBrowserViewModel ContentBrowser { get; } = new ContentBrowserViewModel();

    public LoggerViewModel Logger { get; } = new LoggerViewModel();

    private IRootDock? _layout;
    public IRootDock? Layout
    {
        get => _layout;
        private set
        {
            if (_layout != value)
            {
                _layout = value;
                OnPropertyChanged(nameof(Layout));
            }
        }
    }

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

    public bool IsWorldEditorVisible
    {
        get => _isWorldEditorVisible;
        set
        {
            if (_isWorldEditorVisible != value)
            {
                _isWorldEditorVisible = value;
                OnPropertyChanged(nameof(IsWorldEditorVisible));
                ApplyDockableVisibility(WorldEditor, value);
            }
        }
    }

    public bool IsContentBrowserVisible
    {
        get => _isContentBrowserVisible;
        set
        {
            if (_isContentBrowserVisible != value)
            {
                _isContentBrowserVisible = value;
                OnPropertyChanged(nameof(IsContentBrowserVisible));
                ApplyDockableVisibility(ContentBrowser, value);
            }
        }
    }

    public void ToggleLogger() => IsLoggerVisible = !IsLoggerVisible;
    public void ToggleWorldEditorTab() => IsWorldEditorVisible = !IsWorldEditorVisible;
    public void ToggleContentBrowserTab() => IsContentBrowserVisible = !IsContentBrowserVisible;

    /// <summary>
    /// Add or remove a dockable from <see cref="Layout"/>'s
    /// <c>VisibleDockables</c>. The single DocumentDock is the
    /// parent; toggling the world editor tab means adding/removing
    /// <see cref="WorldEditor"/> from that document list. No-op if
    /// <paramref name="visible"/> matches current state, so
    /// repeated calls don't double-add.
    /// </summary>
    private void ApplyDockableVisibility(Dock.Model.Core.IDockable dockable, bool visible)
    {
        if (Layout == null || dockable == null) return;
        foreach (var rootChild in Layout.VisibleDockables)
        {
            if (rootChild is Dock.Model.Mvvm.Controls.DocumentDock docs)
            {
                bool contains = docs.VisibleDockables.Contains(dockable);
                if (visible && !contains)
                {
                    docs.VisibleDockables.Add(dockable);
                }
                else if (!visible && contains)
                {
                    docs.VisibleDockables.Remove(dockable);
                }
                return;
            }
        }
    }

    public Project? Project
    {
        get => _project;
        set
        {
            if (_project != value)
            {
                if (_project != null) _project.PropertyChanged -= OnProjectPropertyChanged;
                _project = value;
                OnPropertyChanged(nameof(Project));
                OnPropertyChanged(nameof(ProjectPathDisplay));
                OnPropertyChanged(nameof(SceneCountDisplay));
                OnPropertyChanged(nameof(EntityCountDisplay));
                WorldEditor.Project = value;
                ContentBrowser.Project = value;
                if (value != null)
                {
                    value.PropertyChanged += OnProjectPropertyChanged;
                    value.ClearDirty();
                    if (value.ActiveScene != null) value.ActiveScene.ClearDirty();
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

    private void OnProjectPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(GameProject.Project.IsDirty))
        {
            OnPropertyChanged(nameof(WindowTitle));
        }
    }

    public string WindowTitle
    {
        get => _windowTitle;
        set
        {
            var newTitle = value + (_project?.IsDirty == true ? " *" : string.Empty);
            if (_windowTitle != newTitle)
            {
                _windowTitle = newTitle;
                OnPropertyChanged(nameof(WindowTitle));
            }
        }
    }

    private void OnProjectDirtyChanged()
    {
        OnPropertyChanged(nameof(WindowTitle));
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
    public ICommand ToggleWorldEditorTabCommand { get; }
    public ICommand ToggleContentBrowserTabCommand { get; }

    private List<GameEntity> _clipboard = new();

    public MainWindowViewModel()
    {
        var factory = new MainWindowDockFactory(this);
        Layout = factory.CreateLayout();
        factory.InitLayout(Layout!);

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
        ToggleWorldEditorTabCommand = new RelayCommand(_ => ToggleWorldEditorTab());
        ToggleContentBrowserTabCommand = new RelayCommand(_ => ToggleContentBrowserTab());
        CopyCommand = new RelayCommand(_ => CopyExecute(), _ => WorldEditor.SelectedEntities.Length > 0);
        PasteCommand = new RelayCommand(_ => PasteExecute(), _ => _clipboard.Count > 0 && WorldEditor.ActiveScene != null);
        AboutCommand = new RelayCommand(_ => AboutExecute());
    }

    /// <summary>
    /// Replace the current <see cref="Layout"/> with one built
    /// from a previously-saved <see cref="DockingLayout"/>. No-op
    /// if <paramref name="saved"/> is null (first run / older
    /// editor-layout.json without the dock field). The same VM
    /// instances (<see cref="WorldEditor"/>,
    /// <see cref="ContentBrowser"/>) are reused as Dockables, so
    /// Edit-menu commands still operate on the same view models
    /// that DockControl renders as tabs.
    /// </summary>
    public void ApplyLayout(DockingLayout? saved)
    {
        if (saved == null || saved.Entries.Count == 0) return;
        var factory = new MainWindowDockFactory(this);
        Layout = DockingLayoutBridge.ToDockTree(saved, factory, entry => entry.Id switch
        {
            "WorldEditor" => WorldEditor,
            "ContentBrowser" => ContentBrowser,
            _ => null,
        });
        factory.InitLayout(Layout!);
    }

    /// <summary>
    /// Snapshot the current <see cref="Layout"/> to a flat
    /// <see cref="DockingLayout"/> for persistence. Caller writes
    /// the result to <c>EditorLayout.DockLayout</c> and saves
    /// through <see cref="EditorLayoutSettings"/>.
    /// </summary>
    public DockingLayout CaptureLayoutForSave() => DockingLayoutBridge.FromDockTree(Layout!);

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
        using (WorldEditor.UndoRedo.BeginTransaction($"Paste {_clipboard.Count} entity(ies)"))
        {
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
        }
        PrimalEditor_Avalonia.Common.Logger.Info($"Pasted {_clipboard.Count} entity(ies)");
    }

    public async void AboutExecute()
    {
        var topLevel = Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            ? desktop.MainWindow : null;
        if (topLevel != null)
        {
            var dialog = new Views.AboutDialog();
            await dialog.ShowDialog(topLevel);
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
        PrimalEditor_Avalonia.Common.EditorSaveService.Instance.SaveAll(_project);
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
        using (WorldEditor.UndoRedo.BeginTransaction("Add Entity"))
        {
            var action = new AddEntityAction(WorldEditor, entity);
            WorldEditor.UndoRedo.Add(action);
            action.Redo();
        }
        MarkProjectDirty();
    }

    public void DeleteEntityExecute()
    {
        if (WorldEditor.SelectedEntities.Length == 0) return;
        using (WorldEditor.UndoRedo.BeginTransaction($"Delete {WorldEditor.SelectedEntities.Length} entity(ies)"))
        {
            foreach (var entity in WorldEditor.SelectedEntities)
            {
                var action = new RemoveEntityAction(WorldEditor, entity);
                WorldEditor.UndoRedo.Add(action);
                action.Redo();
            }
        }
        MarkProjectDirty();
    }

    public async void RenameEntityExecute()
    {
        if (WorldEditor.SelectedEntities.Length != 1) return;
        var entity = WorldEditor.SelectedEntities[0];
        var dialog = new RenameEntityDialog(entity.Name);
        var window = GetMainWindow();
        if (window != null)
        {
            var result = await dialog.ShowDialog<string?>(window);
            if (!string.IsNullOrEmpty(result))
            {
                var action = new RenameEntityAction(entity, result);
                WorldEditor.UndoRedo.Add(action);
                action.Redo();
                MarkProjectDirty();
            }
        }
    }

    public void DuplicateEntityExecute()
    {
        if (WorldEditor.SelectedEntities.Length == 0) return;
        using (WorldEditor.UndoRedo.BeginTransaction($"Duplicate {WorldEditor.SelectedEntities.Length} entity(ies)"))
        {
            foreach (var entity in WorldEditor.SelectedEntities)
            {
                var action = new DuplicateEntityAction(WorldEditor, entity);
                WorldEditor.UndoRedo.Add(action);
                action.Redo();
            }
        }
        MarkProjectDirty();
    }

    private void MarkProjectDirty()
    {
        if (_project == null) return;
        _project.MarkDirty();
        WorldEditor.ActiveScene?.MarkDirty();
    }

    private Avalonia.Controls.Window? GetMainWindow()
    {
        return Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            ? desktop.MainWindow : null;
    }
}