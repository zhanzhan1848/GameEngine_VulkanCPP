using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.ViewModels;

public class WorldEditorViewModel : ViewModelBase, ISceneEntityContext
{
    private Project? _project;
    private Scene? _activeScene;
    private GameEntity? _selectedEntity;
    private string _debugMessage = string.Empty;

    public UndoRedo UndoRedo { get; } = new UndoRedo();

    public PropertiesViewModel Properties { get; } = new PropertiesViewModel();

    public ObservableCollection<GameEntity> Entities { get; } = new();

    private string _filterText = string.Empty;
    public string FilterText
    {
        get => _filterText;
        set
        {
            if (_filterText != value)
            {
                _filterText = value;
                OnPropertyChanged(nameof(FilterText));
                ApplyFilter(value);
            }
        }
    }

    private System.Collections.Generic.List<GameEntity> _allEntities = new();
    public ObservableCollection<GameEntity> FilteredEntities { get; } = new();

    public void ApplyFilter(string filter)
    {
        if (string.IsNullOrWhiteSpace(filter))
        {
            Entities.Clear();
            foreach (var e in _allEntities) Entities.Add(e);
        }
        else
        {
            var lower = filter.ToLowerInvariant();
            Entities.Clear();
            foreach (var e in _allEntities)
            {
                if (e.Name.ToLowerInvariant().Contains(lower))
                    Entities.Add(e);
            }
        }
    }

    private GameEntity[] _selectedEntities = System.Array.Empty<GameEntity>();
    public GameEntity[] SelectedEntities
    {
        get => _selectedEntities;
        set
        {
            if (_selectedEntities != value)
            {
                _selectedEntities = value;
                OnPropertyChanged(nameof(SelectedEntities));
                SelectedEntity = value.FirstOrDefault();
                Properties.Entities = value;
            }
        }
    }

    public GameEntity? SelectedEntity
    {
        get => _selectedEntity;
        set
        {
            if (_selectedEntity != value)
            {
                _selectedEntity = value;
                OnPropertyChanged(nameof(SelectedEntity));
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
                _project = value;
                Scenes.Clear();
                Entities.Clear();
                if (_project != null)
                {
                    foreach (var scene in _project.Scenes)
                    {
                        Scenes.Add(scene);
                    }
                }
                OnPropertyChanged(nameof(Project));
                OnPropertyChanged(nameof(IsProjectLoaded));
            }
        }
    }

    public bool IsProjectLoaded => _project != null;

    public ObservableCollection<Scene> Scenes { get; } = new();

    public Scene? ActiveScene
    {
        get => _activeScene;
        set
        {
            if (_activeScene != value)
            {
                _activeScene = value;
                _allEntities.Clear();
                Entities.Clear();
                if (_activeScene != null)
                {
                    foreach (var entity in _activeScene.Entities)
                    {
                        _allEntities.Add(entity);
                        Entities.Add(entity);
                    }
                    if (NativeEngineBridge.IsLoaded)
                    {
                        EngineSync.PushScene(_activeScene);
                    }
                }
                OnPropertyChanged(nameof(ActiveScene));
            }
        }
    }

    public string DebugMessage
    {
        get => _debugMessage;
        set
        {
            if (_debugMessage != value)
            {
                _debugMessage = value;
                OnPropertyChanged(nameof(DebugMessage));
            }
        }
    }

    public void AddEntity(GameEntity entity)
    {
        if (entity.Parent == null && _activeScene != null && !_activeScene.Entities.Contains(entity))
        {
            _activeScene.Entities.Add(entity);
            entity.ParentScene = _activeScene;
            Entities.Add(entity);
        }
        else if (entity.Parent != null && !entity.Parent.Children.Contains(entity))
        {
            entity.Parent.Children.Add(entity);
        }
    }

    public void RemoveEntity(GameEntity entity)
    {
        if (entity.Parent != null)
        {
            entity.Parent.Children.Remove(entity);
        }
        else if (entity.ParentScene != null)
        {
            entity.ParentScene.Entities.Remove(entity);
            Entities.Remove(entity);
        }
        entity.Parent = null;
        entity.ParentScene = null;
    }

    public void AddScene(string name = "New Scene")
    {
        if (_project == null) return;
        var scene = new Scene(name);
        _project.Scenes.Add(scene);
        Scenes.Add(scene);
        ActiveScene = scene;
        PrimalEditor_Avalonia.Common.Logger.Info($"Added scene: {name}");
    }

    public void RemoveScene(Scene? scene)
    {
        if (scene == null || _project == null || _project.Scenes.Count <= 1) return;
        _project.Scenes.Remove(scene);
        Scenes.Remove(scene);
        if (ActiveScene == scene)
        {
            ActiveScene = _project.Scenes.FirstOrDefault();
        }
        PrimalEditor_Avalonia.Common.Logger.Info($"Removed scene: {scene.Name}");
    }
}