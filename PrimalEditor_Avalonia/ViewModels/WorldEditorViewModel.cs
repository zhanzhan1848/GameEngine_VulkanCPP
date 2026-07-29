using System;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
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

    public System.Collections.Generic.IReadOnlyList<string> BuildConfigurations { get; } =
        GameProject.Project.BuildConfigurationNames;

    public System.Windows.Input.ICommand BuildProjectCommand { get; } =
        new Common.RelayCommand(_ => BuildProjectStatic());

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
                        // Re-import any entity bound to an asset so the
                        // native mesh content id maps to the entity.
                        // This restores the editor-side _entityNativeIds
                        // table on scene load.
                        _ = LoadEntitiesAsync();
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

    private static void BuildProjectStatic()
    {
        PrimalEditor_Avalonia.Common.Logger.Info(
            "[Build] Placeholder — VS integration not wired in Avalonia build.");
    }

    private void BuildProject()
    {
        var config = _project?.BuildConfigName ?? "(no project)";
        PrimalEditor_Avalonia.Common.Logger.Info(
            $"[Build] Placeholder — would build project in '{config}'. " +
            "(VS integration not wired in Avalonia build.)");
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

    // Native entity id <-> C# GameEntity mapping. Kept here (not on
    // GameEntity) to keep the data model free of platform-specific
    // fields. The view pulls the live list to feed ViewportPicker.
    private readonly System.Collections.Generic.Dictionary<GameEntity, uint> _entityNativeIds = new();

    public void RegisterNativeEntityId(GameEntity entity, uint nativeId)
    {
        if (entity == null || nativeId == 0 || nativeId == ulong.MaxValue) return;
        _entityNativeIds[entity] = nativeId;
        NotifyPickerChanged();
    }

    public uint GetNativeEntityId(GameEntity? entity)
        => entity != null && _entityNativeIds.TryGetValue(entity, out var id) ? id : 0u;

    public System.Collections.Generic.IReadOnlyList<(uint id, PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB bounds)>? GetPickerCandidates()
    {
        if (_activeScene == null) return null;
        if (!NativeEngineBridge.IsLoaded) return null;
        var list = new System.Collections.Generic.List<(uint, PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB)>();
        foreach (var entity in _activeScene.Entities)
        {
            if (!_entityNativeIds.TryGetValue(entity, out var id) || id == 0) continue;
            var pos = NativeEngineBridge.GetEntityPosition(id);
            // Default AABB: 1m cube around the entity's position. We do
            // not query the engine for actual mesh bounds here — that
            // requires a per-entity AABB API the engine currently does
            // not expose. The 1m cube is wide enough to be pickable for
            // the editor's small scenes.
            var half = 0.5f;
            list.Add((id, new PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB(
                pos.x - half, pos.y - half, pos.z - half,
                pos.x + half, pos.y + half, pos.z + half)));
        }
        return list;
    }

    public event System.EventHandler? PickerCandidatesChanged;

    private void NotifyPickerChanged() => PickerCandidatesChanged?.Invoke(this, EventArgs.Empty);

    /// <summary>
    /// Drop an asset from the Content Browser into the active scene.
    /// Supports .mesh: imports the geometry, creates an ECS entity bound
    /// to the resulting content id, and adds it to the scene.
    /// Returns the created entity or null on failure.
    /// </summary>
    public async Task<GameEntity?> DropAssetAsync(AssetInfo info, float posX = 0f, float posY = 0f, float posZ = 0f)
    {
        if (info == null) return null;
        if (_activeScene == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning("DropAsset: no active scene");
            return null;
        }
        var ext = System.IO.Path.GetExtension(info.FullPath).ToLowerInvariant();
        if (ext != ".mesh" && ext != ".obj" && ext != ".fbx" && ext != ".gltf" && ext != ".glb")
        {
            PrimalEditor_Avalonia.Common.Logger.Warning($"DropAsset: unsupported asset type {ext}");
            return null;
        }

        var entityName = System.IO.Path.GetFileNameWithoutExtension(info.FullPath);
        var entity = new GameEntity(entityName)
        {
            AssetPath = info.FullPath,
        };
        entity.Components.Add(new Transform { PositionX = posX, PositionY = posY, PositionZ = posZ });

        if (NativeEngineBridge.IsLoaded)
        {
            var requestId = NativeEngineBridge.ImportMesh(info.FullPath);
            if (requestId == 0)
            {
                PrimalEditor_Avalonia.Common.Logger.Warning("DropAsset: native import rejected the request");
                return null;
            }
            var deadline = System.DateTime.UtcNow.AddSeconds(60);
            while (System.DateTime.UtcNow < deadline)
            {
                NativeEngineBridge.TickMainThreadJobs();
                var (meshId, status) = NativeEngineBridge.TryGetMeshImportResult(requestId);
                if (status != NativeEngineBridge.MeshImportResultStatus.Pending) break;
                await Task.Delay(50);
            }
            var (finalMeshId, finalStatus) = NativeEngineBridge.TryGetMeshImportResult(requestId);
            if (finalStatus != NativeEngineBridge.MeshImportResultStatus.Success)
            {
                PrimalEditor_Avalonia.Common.Logger.Warning($"DropAsset: import failed (status={finalStatus})");
                return null;
            }
            var meshEntityId = NativeEngineBridge.PipelineRegisterMeshEntity(finalMeshId, null);
            if (meshEntityId == 0 || meshEntityId == ulong.MaxValue)
            {
                PrimalEditor_Avalonia.Common.Logger.Warning("DropAsset: PipelineRegisterMeshEntity returned invalid id");
                return null;
            }
            // The native meshEntityId lives in the engine's content system;
            // ECS-side binding happens via EngineSync.PushScene. We do not
            // store the native id on the C# GameEntity to keep the data
            // model free of platform-specific fields.
            RegisterNativeEntityId(entity, (uint)meshEntityId);
        }
        else
        {
            PrimalEditor_Avalonia.Common.Logger.Info("DropAsset: engine not loaded, entity added without native binding");
        }

        AddEntity(entity);
        var action = new AddEntityAction(this, entity);
        UndoRedo.Add(action);
        action.Redo();
        SelectedEntities = new[] { entity };
        PrimalEditor_Avalonia.Common.Logger.Info($"Dropped asset: {info.FileName} → entity {entity.Name}");
        return entity;
    }

    /// <summary>
    /// Re-import every entity in the active scene whose AssetPath is
    /// non-empty. Each successful import produces a fresh native
    /// mesh content id which is registered against the C# entity in
    /// <see cref="_entityNativeIds"/>. The editor's transform gizmo
    /// can then write back to the engine without the user having to
    /// re-drag the asset.
    /// </summary>
    public async Task LoadEntitiesAsync()
    {
        if (_activeScene == null) return;
        if (!NativeEngineBridge.IsLoaded) return;
        foreach (var entity in _activeScene.Entities)
        {
            if (string.IsNullOrEmpty(entity.AssetPath)) continue;
            if (!System.IO.File.Exists(entity.AssetPath))
            {
                PrimalEditor_Avalonia.Common.Logger.Warning($"LoadEntities: asset missing '{entity.AssetPath}'");
                continue;
            }
            await LoadEntityAsync(entity);
        }
    }

    private async Task LoadEntityAsync(GameEntity entity)
    {
        if (!NativeEngineBridge.IsLoaded) return;
        var path = entity.AssetPath;
        var requestId = NativeEngineBridge.ImportMesh(path);
        if (requestId == 0) return;
        var deadline = System.DateTime.UtcNow.AddSeconds(60);
        while (System.DateTime.UtcNow < deadline)
        {
            NativeEngineBridge.TickMainThreadJobs();
            var (meshId, status) = NativeEngineBridge.TryGetMeshImportResult(requestId);
            if (status != NativeEngineBridge.MeshImportResultStatus.Pending) break;
            await System.Threading.Tasks.Task.Delay(50);
        }
        var (finalMeshId, finalStatus) = NativeEngineBridge.TryGetMeshImportResult(requestId);
        if (finalStatus != NativeEngineBridge.MeshImportResultStatus.Success) return;
        var meshEntityId = NativeEngineBridge.PipelineRegisterMeshEntity(finalMeshId, null);
        if (meshEntityId == 0 || meshEntityId == ulong.MaxValue) return;
        RegisterNativeEntityId(entity, (uint)meshEntityId);
        // Apply the entity's current transform (so the user sees the
        // mesh at the location they saved it at).
        var t = entity.GetTransform();
        if (t != null)
        {
            float[] pos = { t.PositionX, t.PositionY, t.PositionZ };
            float[] rot = { t.RotationX, t.RotationY, t.RotationZ };
            float[] scl = { t.ScaleX, t.ScaleY, t.ScaleZ };
            NativeEngineBridge.SetEntityTransform((uint)meshEntityId, pos, rot, scl);
        }
        NotifyPickerChanged();
    }
}