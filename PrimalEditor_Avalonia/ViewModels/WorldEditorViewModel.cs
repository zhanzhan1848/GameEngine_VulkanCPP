using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Dock.Controls.DeferredContentControl;
using Dock.Model.Mvvm.Controls;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Entities;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.ViewModels;

public class WorldEditorViewModel : Document, ISceneEntityContext, IDeferredContentPresentation
{
    // Dock 12.1 延迟呈现（reveal 时间线）在真实窗口下不刷新内容区，声明同步呈现。
    public bool DeferContentPresentation => false;
    private int _bottomPanelIndex;
    /// <summary>Local bottom-panel tab; changing it never replaces or resizes the viewport.</summary>
    public int BottomPanelIndex
    {
        get => _bottomPanelIndex;
        set
        {
            var index = Math.Clamp(value, 0, 1);
            if (_bottomPanelIndex == index) return;
            _bottomPanelIndex = index;
            OnPropertyChanged(nameof(BottomPanelIndex));
        }
    }

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

    // =======================================================================
    // Viewport render mode (plan §6.1 B3)
    // =======================================================================

    /// <summary>Toolbar labels for the render mode ComboBox; index maps
    /// 1:1 onto NativeEngineBridge.PipelineApplyModePreset's mode ids.</summary>
    public System.Collections.Generic.IReadOnlyList<string> RenderModeOptions { get; } = new[]
    {
        "Forward",
        "Deferred",
        "Deferred + Shadow + AO",
        "Deferred + Shadow + AO + GI",
    };

    private int _editorRenderModeIndex = 3;

    /// <summary>Selected index into <see cref="RenderModeOptions"/>. The
    /// engine default (deferred + shadow + AO + GI) is index 3; applying
    /// index 3 natively is redundant on a fresh viewport, so the bridge
    /// call only fires when the user actually changes the selection.</summary>
    public int EditorRenderModeIndex
    {
        get => _editorRenderModeIndex;
        set
        {
            if (value < 0 || value >= RenderModeOptions.Count) return;
            if (_editorRenderModeIndex != value)
            {
                _editorRenderModeIndex = value;
                OnPropertyChanged(nameof(EditorRenderModeIndex));
                if (NativeEngineBridge.IsLoaded)
                {
                    NativeEngineBridge.PipelineApplyModePreset((uint)value);
                    PrimalEditor_Avalonia.Common.Logger.Info(
                        $"[Render] viewport mode → {RenderModeOptions[value]}");
                }
            }
        }
    }

    // =======================================================================
    // Viewport view mode (plan §6.3 A7)
    // =======================================================================

    /// <summary>Toolbar labels for the view-mode ComboBox; index maps onto
    /// NativeEngineBridge.PipelineSetViewMode (0=Shaded, 1=Wireframe;
    /// 2=Unlit reserved engine-side, not exposed yet).</summary>
    public System.Collections.Generic.IReadOnlyList<string> ViewModeOptions { get; } = new[]
    {
        "Shaded",
        "Wireframe",
    };

    private int _viewModeIndex;

    /// <summary>Selected view mode. Shaded is the engine default, so the
    /// bridge call only fires when the user selects Wireframe (or switches
    /// back explicitly).</summary>
    public int ViewModeIndex
    {
        get => _viewModeIndex;
        set
        {
            if (value < 0 || value >= ViewModeOptions.Count) return;
            if (_viewModeIndex != value)
            {
                _viewModeIndex = value;
                OnPropertyChanged(nameof(ViewModeIndex));
                if (NativeEngineBridge.IsLoaded)
                {
                    NativeEngineBridge.PipelineSetViewMode((uint)value);
                    PrimalEditor_Avalonia.Common.Logger.Info(
                        $"[Render] view mode → {ViewModeOptions[value]}");
                }
            }
        }
    }

    // =======================================================================
    // Run target graphics API (plan §Run/Stop: 分图形API / 分debug/release)
    // =======================================================================

    public System.Collections.Generic.IReadOnlyList<string> TargetGraphicsApis { get; } =
        new[] { "Metal", "Vulkan" };

    private int _targetGraphicsApiIndex;

    /// <summary>Graphics API the Run button targets. Metal = Darwin/Debug|Release
    /// trees, Vulkan = Darwin/DebugVulkan tree; the mapping lives in
    /// RuntimeLauncher.ResolvePlayerDir.</summary>
    public int TargetGraphicsApiIndex
    {
        get => _targetGraphicsApiIndex;
        set
        {
            if (value < 0 || value >= TargetGraphicsApis.Count) return;
            if (_targetGraphicsApiIndex != value)
            {
                _targetGraphicsApiIndex = value;
                OnPropertyChanged(nameof(TargetGraphicsApiIndex));
                OnPropertyChanged(nameof(TargetGraphicsApi));
            }
        }
    }

    public string TargetGraphicsApi => TargetGraphicsApis[_targetGraphicsApiIndex];

    // =======================================================================
    // Run 模式（§Run 三选一）：独立窗口（原路线 PrimalPlayer）/ 固定分辨率
    // （PrimalPlayer --width/--height/--fixed，确定性录制用）/ 渲染表面
    // （不开外部进程，编辑器内嵌 surface 切 runtime 渲染路径）。
    // =======================================================================

    /// <summary>工具栏 Run 模式标签；index 1:1 映射 RuntimeLauncher.RunMode。</summary>
    public System.Collections.Generic.IReadOnlyList<string> RunModeOptions { get; } = new[]
    {
        "Player Window",
        "Fixed Resolution",
        "Render Surface",
    };

    private int _runModeIndex;

    /// <summary>当前 Run 模式。默认 0 = 原路线（外部 PrimalPlayer 默认窗口）。</summary>
    public int RunModeIndex
    {
        get => _runModeIndex;
        set
        {
            var clamped = Math.Clamp(value, 0, RunModeOptions.Count - 1);
            if (_runModeIndex != clamped)
            {
                _runModeIndex = clamped;
                OnPropertyChanged(nameof(RunModeIndex));
                OnPropertyChanged(nameof(IsFixedResolutionMode));
            }
        }
    }

    /// <summary>固定分辨率模式的 W/H 输入框只在该模式下可见（AXAML IsVisible 绑定）。</summary>
    public bool IsFixedResolutionMode => _runModeIndex == 1;

    private int _runWidth = 1280;

    /// <summary>固定分辨率模式的渲染/窗口宽度（像素，16..8192）。</summary>
    public int RunWidth
    {
        get => _runWidth;
        set
        {
            var clamped = Math.Clamp(value, 16, 8192);
            if (_runWidth != clamped)
            {
                _runWidth = clamped;
                OnPropertyChanged(nameof(RunWidth));
            }
        }
    }

    private int _runHeight = 720;

    /// <summary>固定分辨率模式的渲染/窗口高度（像素，16..8192）。</summary>
    public int RunHeight
    {
        get => _runHeight;
        set
        {
            var clamped = Math.Clamp(value, 16, 8192);
            if (_runHeight != clamped)
            {
                _runHeight = clamped;
                OnPropertyChanged(nameof(RunHeight));
            }
        }
    }

    // =======================================================================
    // Run 录制（Part B，Docs/2026-09-05）：Run 启动 PrimalPlayer 时附带逐帧
    // 录制参数。录制本体在 player 进程（渲染帧与捕获帧 1:1，无丢帧），编辑器
    // 只拼参数；产物目录约定见 RuntimeLauncher.ResolveRecordDirectory。
    // =======================================================================

    private bool _recordOnRun;

    /// <summary>Run 时对 PrimalPlayer 开启逐帧 PNG 录制（--record）。</summary>
    public bool RecordOnRun
    {
        get => _recordOnRun;
        set
        {
            if (_recordOnRun != value)
            {
                _recordOnRun = value;
                OnPropertyChanged(nameof(RecordOnRun));
            }
        }
    }

    private int _recordFrames = 300;

    /// <summary>录制捕获帧上限（录满后 player 自动退出）；0 = 跟随窗口关闭。</summary>
    public int RecordFrames
    {
        get => _recordFrames;
        set
        {
            var clamped = Math.Clamp(value, 0, 100_000);
            if (_recordFrames != clamped)
            {
                _recordFrames = clamped;
                OnPropertyChanged(nameof(RecordFrames));
            }
        }
    }

    private bool _recordMetrics = true;

    /// <summary>录制时同时写逐帧亮度遥测 CSV（--record-metrics，口径同 editor.metrics）。</summary>
    public bool RecordMetrics
    {
        get => _recordMetrics;
        set
        {
            if (_recordMetrics != value)
            {
                _recordMetrics = value;
                OnPropertyChanged(nameof(RecordMetrics));
            }
        }
    }

    // =======================================================================
    // Gizmo snapping (plan §6.1 A2)
    // =======================================================================

    public SnapSettings SnapSettings { get; } = new();

    // =======================================================================
    // Hierarchy reparent (plan §6.1 A3)
    // =======================================================================

    /// <summary>
    /// Move <paramref name="entity"/> under <paramref name="newParent"/>
    /// (null = scene root). Rejects self-parenting and moving an entity
    /// into its own subtree. Document-model only: the engine has no
    /// parent/child transform notion (SetEntityTransform is world-space),
    /// so no transform compensation is needed and no native push is made.
    /// </summary>
    public bool ReparentEntity(GameEntity entity, GameEntity? newParent)
    {
        if (entity == null) return false;
        if (ReferenceEquals(entity, newParent)) return false;
        if (newParent != null && IsDescendantOf(newParent, entity)) return false;
        var oldParent = entity.Parent;
        if (ReferenceEquals(oldParent, newParent)) return false;

        if (oldParent != null) oldParent.Children.Remove(entity);
        else
        {
            _activeScene?.Entities.Remove(entity);
            Entities.Remove(entity);
        }

        if (newParent != null)
        {
            newParent.Children.Add(entity);
        }
        else
        {
            if (_activeScene != null && !_activeScene.Entities.Contains(entity))
                _activeScene.Entities.Add(entity);
            if (!Entities.Contains(entity)) Entities.Add(entity);
        }
        entity.Parent = newParent;

        // Keep the filtered tree consistent (a filtered-in root may have
        // become a child, and vice versa).
        ApplyFilter(FilterText);
        MarkProjectDirty();
        return true;
    }

    private static bool IsDescendantOf(GameEntity candidate, GameEntity ancestor)
    {
        for (var p = candidate.Parent; p != null; p = p.Parent)
        {
            if (ReferenceEquals(p, ancestor)) return true;
        }
        return false;
    }

    public Scene? ActiveScene
    {
        get => _activeScene;
        set
        {
            if (_activeScene != value)
            {
                // Single sync path: SceneRuntimeSynchronizer. Earlier
                // iterations also called EngineSync.PushScene and
                // LoadEntitiesAsync here, but those duplicated the
                // creation of native entities (one engine entity per
                // C# GameEntity x2) and left ghost IDs behind on
                // teardown. Anything that needs the legacy
                // CreateGameEntity path should call those APIs
                // explicitly during creation, never from a scene-swap.
                SceneSynchronizer.Teardown();
                _activeScene = value;
                SelectedEntities = System.Array.Empty<GameEntity>();
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
                        SceneSynchronizer.Sync(_activeScene);
                    }
                    SceneEnvironmentViewModel.Attach(_activeScene.Environment);
                }
                else
                {
                    SceneEnvironmentViewModel.Attach(null);
                }
                OnPropertyChanged(nameof(ActiveScene));
            }
        }
    }

    internal RuntimeEntityMap RuntimeEntityMap { get; } = new();
    internal RuntimeAssetMap RuntimeAssetMap { get; } = new();
    internal RuntimeMaterialMap RuntimeMaterialMap { get; } = new();
    internal SceneRuntimeSynchronizer SceneSynchronizer { get; }

    /// <summary>
    /// Toolbar data source. Bound by <c>WorldEditorView.axaml</c>'s
    /// <c>ItemsControl</c> so each entry renders one toolbar button.
    /// Adding a new entity kind = one row in <see cref="EntityFactory"/>;
    /// no view edits required.
    /// </summary>
    public IReadOnlyList<EntityFactoryDescriptor> EntityFactoryItems => EntityFactory.All;

    /// <summary>
    /// Inspector-side view model for scene environment (ambient / fog /
    /// skybox / tone mapping). Bound by <c>EnvironmentView.axaml</c>;
    /// forwards every property change to the native engine via
    /// <see cref="NativeEngineBridge.UpdateSceneEnvironment"/>.
    /// </summary>
    public SceneEnvironmentViewModel SceneEnvironmentViewModel { get; } = new();

    public WorldEditorViewModel()
    {
        Properties.SceneEnvironment = SceneEnvironmentViewModel;
        SceneSynchronizer = new SceneRuntimeSynchronizer(
            new NativeEngineBridgeAdapter(),
            RuntimeEntityMap,
            RuntimeAssetMap,
            RuntimeMaterialMap);
        Properties.PushTransformsToNativeCallback = PushTransformsToNative;
        Properties.PushMaterialsToNativeCallback = entities =>
        {
            foreach (var entity in entities) SceneSynchronizer.RefreshEntityMaterials(entity);
        };
        Properties.PushMeshesToNativeCallback = entities =>
        {
            foreach (var entity in entities) SceneSynchronizer.RefreshEntityMesh(entity);
        };
        Properties.MarkProjectDirtyCallback = MarkProjectDirty;
        Properties.UndoRedoQuery = () => UndoRedo;
        // 粒子预设切换 = 移除旧发射器 + 按当前组件重建（plan §6.3 B4 P2）。
        Properties.PushParticlesRebuildCallback = entity =>
        {
            var nativeId = GetNativeEntityId(entity);
            if (nativeId != 0) NativeEngineBridge.EmitterRemoveFromEntity(nativeId);
            if (NativeEngineBridge.IsLoaded) SceneSynchronizer.SyncEntity(entity);
        };
        // A6: PCG 编辑器"应用到场景"——把图的输出点批量落成场景实体
        //（文档模型优先：走 AddEntity + 单事务 undo + SyncEntity，绝不裸调
        // 引擎建第二事实源）。
        Editors.Pcg.PcgEditorViewModel.ApplyToSceneCallback = payload =>
        {
            if (Project == null || ActiveScene == null) return 0;
            return ApplyPcgPointsToScene(payload);
        };
        Properties.ParticleSavePresetCallback = name =>
        {
            if (Project == null) return;
            var safe = string.Join('_', name.Split(
                System.IO.Path.GetInvalidFileNameChars(), StringSplitOptions.RemoveEmptyEntries));
            var path = System.IO.Path.Combine(Project.Path, safe + ".particle.json");
            if (PrimalEditor_Avalonia.Native.NativeEngineBridge.ParticlePresetSaveToFile(name, path))
            {
                PrimalEditor_Avalonia.Common.Logger.Info($"[Particle] preset saved: {path}");
                Properties.RefreshParticlePresetOptions();
            }
        };
        // Inspector "Open Editor…": open the preset in the material-style
        // asset editor window; built-in presets (fire/smoke/…) have no file
        // yet, so export the engine's current config to disk on first open.
        Properties.ParticleOpenEditorCallback = name =>
        {
            if (Project == null)
            {
                PrimalEditor_Avalonia.Common.Logger.Warning("[Particle] open editor: no project open");
                return;
            }
            var safe = string.Join('_', name.Split(
                System.IO.Path.GetInvalidFileNameChars(), StringSplitOptions.RemoveEmptyEntries));
            var path = System.IO.Path.Combine(Project.Path, safe + ".particle.json");
            if (!System.IO.File.Exists(path))
            {
                if (!PrimalEditor_Avalonia.Native.NativeEngineBridge.ParticlePresetSaveToFile(name, path))
                {
                    PrimalEditor_Avalonia.Common.Logger.Warning($"[Particle] preset export failed: {name}");
                    return;
                }
                PrimalEditor_Avalonia.Common.Logger.Info($"[Particle] preset exported for editing: {path}");
                Properties.RefreshParticlePresetOptions();
            }
            Editors.AssetEditorRegistry.Open(new Content.AssetInfo
            {
                FullPath = path,
                Type = PrimalEditor_Avalonia.Native.AssetType.ParticlePreset,
            });
        };
        // Inspector PARTICLE FX "Open Editor…": open the .pfx_emitter asset in
        // the module-stack editor window (the PFX framework's editing surface).
        Properties.PfxOpenEditorCallback = assetPath =>
        {
            if (Project == null)
            {
                PrimalEditor_Avalonia.Common.Logger.Warning("[ParticleFX] open editor: no project open");
                return;
            }
            if (!System.IO.File.Exists(assetPath))
            {
                PrimalEditor_Avalonia.Common.Logger.Warning($"[ParticleFX] open editor: asset missing '{assetPath}'");
                return;
            }
            Editors.AssetEditorRegistry.Open(new Content.AssetInfo
            {
                FullPath = assetPath,
                Type = PrimalEditor_Avalonia.Native.AssetType.PfxEmitter,
            });
        };
        // Material editors are free-floating windows; their saves reach
        // the live scene through this static event.
        SceneRuntimeSynchronizer.MaterialSavedOnDisk += OnMaterialSavedOnDisk;
    }

    private void OnMaterialSavedOnDisk(AssetGuid guid) => SceneSynchronizer.ReloadMaterial(guid);

    /// <summary>
    /// Create a new entity from the given factory descriptor, parent
    /// it at the scene root, register
    /// undo, and push the entity to the native engine. Returns the
    /// new entity on success, null if the scene is missing.
    /// </summary>
    public GameEntity? CreateEntityFromFactory(EntityFactoryDescriptor descriptor)
    {
        if (descriptor == null) return null;
        if (ActiveScene == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"Cannot add {descriptor.DisplayName}: no active scene. Open a project first.");
            return null;
        }

        var entity = descriptor.CreateEntity();
        // Creation buttons always add a root entity. Using the current
        // selection as an implicit parent made every subsequent creation a
        // child of the object created immediately before it (because creation
        // also selects the new object). Parenting must be an explicit hierarchy
        // operation, never a side effect of selection.
        AddEntity(entity);
        UndoRedo.Add(new AddEntityAction(this, entity));
        SelectedEntities = new[] { entity };
        MarkProjectDirty();

        if (NativeEngineBridge.IsLoaded)
        {
            SceneSynchronizer.SyncEntity(entity);
        }

        PrimalEditor_Avalonia.Common.Logger.Info($"Added {descriptor.DisplayName}: {entity.Name}");
        return entity;
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
		var wasAdded = false;
        if (entity.Parent == null && _activeScene != null && !_activeScene.Entities.Contains(entity))
        {
            _activeScene.Entities.Add(entity);
            entity.ParentScene = _activeScene;
            if (!_allEntities.Contains(entity)) _allEntities.Add(entity);
            if (!Entities.Contains(entity)) Entities.Add(entity);
			wasAdded = true;
        }
        else if (entity.Parent != null && !entity.Parent.Children.Contains(entity))
        {
            entity.Parent.Children.Add(entity);
			wasAdded = true;
        }
		if (wasAdded && NativeEngineBridge.IsLoaded) SceneSynchronizer.SyncEntity(entity);
        MarkProjectDirty();
    }

    public void RemoveEntity(GameEntity entity)
    {
		SceneSynchronizer.RemoveEntity(entity);
        if (entity.Parent != null)
        {
            entity.Parent.Children.Remove(entity);
        }
        else if (entity.ParentScene != null)
        {
            entity.ParentScene.Entities.Remove(entity);
            Entities.Remove(entity);
            _allEntities.Remove(entity);
        }
        entity.Parent = null;
        entity.ParentScene = null;
        MarkProjectDirty();
    }

	/// <summary>
	/// Delete the current hierarchy selection as one undoable operation.
	/// Toolbar, menu and keyboard entry points all route through this method.
	/// </summary>
	public int DeleteSelectedEntities()
	{
		var selected = SelectedEntities
			.Where(entity => entity != null)
			.Distinct()
			.ToArray();
		if (selected.Length == 0) return 0;

		using (UndoRedo.BeginTransaction($"Delete {selected.Length} entity(ies)"))
		{
			foreach (var entity in selected)
			{
				var action = new RemoveEntityAction(this, entity);
				UndoRedo.Add(action);
				action.Redo();
			}
		}
		SelectedEntities = System.Array.Empty<GameEntity>();
		return selected.Length;
	}

    public void AddScene(string name = "New Scene")
    {
        if (_project == null) return;
        var scene = new Scene(name);
        // Default sun light so new scenes are lit without manual setup;
        // ActiveScene = scene below pushes it to the engine via Sync().
        scene.Entities.Add(SceneDefaults.CreateSunLight());
        _project.Scenes.Add(scene);
        Scenes.Add(scene);
        ActiveScene = scene;
        MarkProjectDirty();
        PrimalEditor_Avalonia.Common.Logger.Info($"Added scene: {name}");
    }

    private static void BuildProjectStatic()
    {
        var current = ProjectContextLocator.Current;
        if (current == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning("[Build] No active project; skipping.");
            return;
        }
        BuildPipeline.ExecuteRebuild(current);
    }

    private void BuildProject()
    {
        var config = _project?.BuildConfigName ?? "(no project)";
        PrimalEditor_Avalonia.Common.Logger.Info($"[Build] Starting rebuild in '{config}'.");
        BuildPipeline.ExecuteRebuild(_project!);
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
        MarkProjectDirty();
        PrimalEditor_Avalonia.Common.Logger.Info($"Removed scene: {scene.Name}");
    }

    public void MarkProjectDirty()
    {
        if (_project == null) return;
        _project.MarkDirty();
        _activeScene?.MarkDirty();
    }

    /// <summary>
    /// Push the current Transform values of the given entities into
    /// the native engine. Called by the gizmo drag handler and the
    /// inspector after they mutate the C# source of truth.
    /// </summary>
    public void PushTransformsToNative(IEnumerable<GameEntity> entities)
    {
        SceneSynchronizer.PushTransforms(entities);
    }

    // Native entity id <-> C# GameEntity mapping. RuntimeEntityMap is the
// single source of truth — both SceneRuntimeSynchronizer and the legacy
// async LoadEntityAsync path write into it, so GetPickerCandidates and
// the write-back paths (gizmo, inspector) can never drift on different
// dictionaries.
public void RegisterNativeEntityId(GameEntity entity, uint nativeId)
{
    if (entity == null || nativeId == 0 || nativeId == ulong.MaxValue) return;
    RuntimeEntityMap.Register(entity.EntityGuid, nativeId);
    NotifyPickerChanged();
}

public uint GetNativeEntityId(GameEntity? entity)
{
    if (entity == null) return 0u;
    if (entity.EntityGuid.Value == Guid.Empty) return 0u;
    return RuntimeEntityMap.GetEntityId(entity.EntityGuid) ?? 0u;
}

    public System.Collections.Generic.IReadOnlyList<(uint id, PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB bounds)>? GetPickerCandidates()
    {
        if (_activeScene == null) return null;
        if (!NativeEngineBridge.IsLoaded) return null;
        var list = new System.Collections.Generic.List<(uint, PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB)>();
        foreach (var entity in EnumerateSceneEntities(_activeScene.Entities))
        {
            var id = GetNativeEntityId(entity);
            if (id == 0) continue;
            if (NativeEngineBridge.TryGetEntityWorldAABB(id, out var b))
            {
                list.Add((id, new PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB(
                    b.minX, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ)));
            }
        }
        return list;
    }

    private static IEnumerable<GameEntity> EnumerateSceneEntities(IEnumerable<GameEntity> roots)
    {
        foreach (var entity in roots)
        {
            yield return entity;
            foreach (var child in EnumerateSceneEntities(entity.Children))
                yield return child;
        }
    }

    // =======================================================================
    // A7 视图模式: show-all-bounds overlay
    // =======================================================================

    /// <summary>Toolbar "Bounds" toggle — draw dim AABB wireframes for every
    /// mesh-bearing entity (not just the selection). The per-frame AABB
    /// query only runs while this is on.</summary>
    public bool ShowAllBounds
    {
        get => _showAllBounds;
        set
        {
            if (_showAllBounds != value)
            {
                _showAllBounds = value;
                OnPropertyChanged(nameof(ShowAllBounds));
                PrimalEditor_Avalonia.Common.Logger.Info(
                    $"[Render] show-all bounds → {value}");
            }
        }
    }
    private bool _showAllBounds;

    /// <summary>AABBs of every mesh-bearing entity, for the viewport's
    /// AllBoundsProvider. Empty while <see cref="ShowAllBounds"/> is off or
    /// the bridge is down.</summary>
    public System.Collections.Generic.IReadOnlyList<PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB> GetAllEntityBounds()
    {
        if (!_showAllBounds || _activeScene == null || !NativeEngineBridge.IsLoaded)
            return System.Array.Empty<PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB>();
        var cands = GetPickerCandidates();
        if (cands == null || cands.Count == 0)
            return System.Array.Empty<PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB>();
        var list = new System.Collections.Generic.List<PrimalEditor_Avalonia.Views.Engine3D.ViewportPicker.AABB>(cands.Count);
        foreach (var c in cands) list.Add(c.bounds);
        return list;
    }

    public event System.EventHandler? PickerCandidatesChanged;

    private void NotifyPickerChanged() => PickerCandidatesChanged?.Invoke(this, EventArgs.Empty);

    /// <summary>
    /// Drop an asset from the Content Browser into the active scene.
    /// Supports .mesh / .fbx / .obj / .gltf / .glb / .model.json: imports
    /// the geometry, creates an ECS entity bound to the resulting content
    /// id, and adds it to the scene. Model containers (.model.json) drop
    /// their cooked .mesh companion so a single entity renders the whole
    /// model. Returns the created entity or null on failure.
    /// </summary>
    public async Task<GameEntity?> DropAssetAsync(AssetInfo info, float posX = 0f, float posY = 0f, float posZ = 0f)
    {
        if (info == null) return null;
        if (_activeScene == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning("DropAsset: no active scene");
            return null;
        }
        // Prefab instantiation (plan §6.2 A4) takes priority over the mesh
        // drop path — .prefab assets never resolve through ResolveDropTarget.
        if (info.Type == AssetType.Prefab ||
            info.FullPath.EndsWith(".prefab", StringComparison.OrdinalIgnoreCase))
        {
            return await InstantiatePrefabAsync(info, posX, posY, posZ);
        }
        // B4 P4: particle preset drop — create an entity with a particle
        // emitter bound to the preset (no mesh import involved).
        if (info.Type == AssetType.ParticlePreset ||
            info.FullPath.EndsWith(".particle.json", StringComparison.OrdinalIgnoreCase))
        {
            return CreateParticlePresetEntity(info, posX, posY, posZ);
        }
        // PFX programmable emitter drop — attach to the selected entity when
        // one exists (author-then-apply workflow), otherwise spawn a fresh
        // emitter entity at the drop point.
        if (info.Type == AssetType.PfxEmitter ||
            info.FullPath.EndsWith(".pfx_emitter", StringComparison.OrdinalIgnoreCase))
        {
            if (SelectedEntities.Length == 1 && ApplyPfxToSelectedEntity(info))
                return SelectedEntities[0];
            return CreatePfxEntity(info, posX, posY, posZ);
        }
        var (nativeMeshPath, entityName) = ResolveDropTarget(info);
        if (nativeMeshPath == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning($"DropAsset: unsupported asset type {info.FullPath}");
            return null;
        }

        var entity = new GameEntity(entityName);
        entity.Components.Add(new Transform { PositionX = posX, PositionY = posY, PositionZ = posZ });
        var meshRenderer = new MeshRendererComponent { MeshGuid = new AssetGuid(info.Guid) };
        TryAssignDefaultMaterial(meshRenderer, info.FullPath);
        entity.Components.Add(meshRenderer);

        if (NativeEngineBridge.IsLoaded)
        {
            var requestId = NativeEngineBridge.ImportMesh(nativeMeshPath);
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
            // Pre-register the cooked content id so SceneSynchronizer.SyncEntity
            // can pick it up without re-importing the asset. The import above
            // already paid the cost; reusing the result here keeps a single
            // mesh registered against the AssetGuid.
            SceneSynchronizer.RegisterPreImportedMesh(
                new AssetGuid(info.Guid), finalMeshId);
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
        // SyncEntity does the actual native push (entity, mesh bind with
        // the default white PBR material, picker registration). Replacing
        // the old direct PipelineRegisterMeshEntity call closes the gap
        // where dropped assets rendered with no material.
        if (NativeEngineBridge.IsLoaded) SceneSynchronizer.SyncEntity(entity);
        PrimalEditor_Avalonia.Common.Logger.Info($"Dropped asset: {info.FileName} → entity {entity.Name}");
        return entity;
    }

    /// <summary>
    /// B4 P4: create an entity from a .particle.json preset (drag-drop or
    /// future menu entry). Transform at the drop position + a particle
    /// emitter bound to the preset file stem; standard AddEntity undo flow
    /// then SyncEntity pushes it to the engine.
    /// </summary>
    private GameEntity? CreateParticlePresetEntity(AssetInfo info, float posX, float posY, float posZ)
    {
        var presetName = System.IO.Path.GetFileNameWithoutExtension(info.FullPath);
        // 自定义预设先进引擎的 preset 系统，否则 SyncEntity 的
        // PresetApplyToConfig 不认识这个名字而回落 "fire"。
        if (NativeEngineBridge.IsLoaded)
            NativeEngineBridge.ParticlePresetLoadFromFile(info.FullPath);
        var entity = new GameEntity(presetName);
        entity.Components.Add(new Transform { PositionX = posX, PositionY = posY, PositionZ = posZ });
        entity.Components.Add(new PrimalEditor_Avalonia.Components.ParticleEmitterComponent
        {
            PresetName = presetName,
        });

        AddEntity(entity);
        var action = new AddEntityAction(this, entity);
        UndoRedo.Add(action);
        action.Redo();
        SelectedEntities = new[] { entity };
        if (NativeEngineBridge.IsLoaded) SceneSynchronizer.SyncEntity(entity);
        PrimalEditor_Avalonia.Common.Logger.Info(
            $"[Particle] preset dropped: {info.FileName} → entity {entity.Name}");
        return entity;
    }

    /// <summary>
    /// B4 P4: apply a .particle.json preset (content-browser context menu) to
    /// the single selected entity's particle emitter. Loads the file into the
    /// preset system first (custom presets are unknown to the engine until
    /// loaded), then routes through the inspector's preset switch = remove
    /// old emitter + rebuild. Returns false (with a log) when the selection
    /// has no particle emitter.
    /// </summary>
    /// <summary>
    /// PFX author-then-apply: attach a .pfx_emitter asset to the single
    /// selected entity — adds a ParticleFX component when missing, otherwise
    /// retargets AssetPath (the sync path destroys nothing: the component's
    /// emitter is recreated on the next SyncEntity after EmitterRemove).
    /// Returns false (with a log) when the workflow preconditions fail.
    /// </summary>
    public bool ApplyPfxToSelectedEntity(PrimalEditor_Avalonia.Content.AssetInfo info)
    {
        if (info == null) return false;
        if (SelectedEntities.Length != 1)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                "[PFX] apply: select exactly one entity");
            return false;
        }
        var entity = SelectedEntities[0];
        var pfx = entity.GetComponent<PrimalEditor_Avalonia.Components.ParticleFXComponent>();
        if (pfx == null)
        {
            pfx = new PrimalEditor_Avalonia.Components.ParticleFXComponent();
            entity.Components.Add(pfx);
        }
        pfx.AssetPath = info.FullPath;
        if (NativeEngineBridge.IsLoaded) SceneSynchronizer.SyncEntity(entity);
        MarkProjectDirty();
        PrimalEditor_Avalonia.Common.Logger.Info(
            $"[PFX] asset applied: {info.FileName} → entity '{entity.Name}'");
        return true;
    }

    /// <summary>PFX drop onto empty space: create an emitter entity bound to
    /// the asset (same flow as the B4 preset drop).</summary>
    private GameEntity? CreatePfxEntity(AssetInfo info, float posX, float posY, float posZ)
    {
        var entityName = Path.GetFileNameWithoutExtension(info.FullPath.Replace(".pfx_emitter", ""));
        var entity = new GameEntity(entityName);
        entity.Components.Add(new Transform { PositionX = posX, PositionY = posY, PositionZ = posZ });
        entity.Components.Add(new PrimalEditor_Avalonia.Components.ParticleFXComponent
        {
            AssetPath = info.FullPath,
        });

        AddEntity(entity);
        var action = new AddEntityAction(this, entity);
        UndoRedo.Add(action);
        action.Redo();
        SelectedEntities = new[] { entity };
        if (NativeEngineBridge.IsLoaded) SceneSynchronizer.SyncEntity(entity);
        PrimalEditor_Avalonia.Common.Logger.Info(
            $"[PFX] asset dropped: {info.FileName} → entity {entity.Name}");
        return entity;
    }

    /// <summary>
    /// B4 P4: apply a .particle.json preset (content-browser context menu) to
    /// the single selected entity's particle emitter. Loads the file into the
    /// preset system first (custom presets are unknown to the engine until
    /// loaded), then routes through the inspector's preset switch = remove
    /// old emitter + rebuild. Returns false (with a log) when the selection
    /// has no particle emitter.
    /// </summary>
    public bool ApplyPresetToSelectedParticleEntity(PrimalEditor_Avalonia.Content.AssetInfo info)
    {
        if (info == null) return false;
        if (SelectedEntities.Length != 1)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                "[Particle] apply preset: select exactly one entity");
            return false;
        }
        if (SelectedEntities[0].GetComponent<PrimalEditor_Avalonia.Components.ParticleEmitterComponent>() == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"[Particle] apply preset: entity '{SelectedEntities[0].Name}' has no particle emitter");
            return false;
        }
        var presetName = System.IO.Path.GetFileNameWithoutExtension(info.FullPath);
        if (NativeEngineBridge.IsLoaded && !NativeEngineBridge.ParticlePresetLoadFromFile(info.FullPath))
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"[Particle] preset load failed: {info.FullPath} (applying by name anyway)");
        }
        // 穿 Inspector 的预设切换路径：PresetName setter 拆旧发射器并重建。
        Properties.ParticlePresetName = presetName;
        Properties.RefreshParticlePresetOptions();
        return true;
    }

    /// <summary>
    /// Instantiate a .prefab asset: read the document, rebuild the entity
    /// subtree with fresh guids, attach/keep PrefabInstanceComponent on the
    /// roots, register everything as one undoable transaction. The drop
    /// position lands on the first root; children keep relative transforms.
    /// </summary>
    public async Task<GameEntity?> InstantiatePrefabAsync(AssetInfo info, float posX, float posY, float posZ)    {
        var prefab = PrimalEditor_Avalonia.Content.Prefabs.PrefabSerializer.Read(info.FullPath);
        if (prefab == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning($"[Prefab] unreadable prefab: {info.FullPath}");
            return null;
        }
        var roots = GameProject.Serializer.InstantiateEntityDocuments(prefab.Entities, reassignGuids: true);
        if (roots.Count == 0)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning($"[Prefab] prefab has no entities: {info.FullPath}");
            return null;
        }

        if (roots[0].GetTransform() is { } rootTransform)
        {
            rootTransform.PositionX = posX;
            rootTransform.PositionY = posY;
            rootTransform.PositionZ = posZ;
        }

        using (UndoRedo.BeginTransaction($"Instantiate prefab '{prefab.Name}'"))
        {
            foreach (var root in roots)
            {
                if (root.GetComponent<Components.PrefabInstanceComponent>() == null)
                {
                    root.Components.Add(new Components.PrefabInstanceComponent
                    {
                        PrefabGuid = new AssetGuid(info.Guid),
                    });
                }
                AddEntity(root);
                var action = new AddEntityAction(this, root);
                UndoRedo.Add(action);
                action.Redo();
            }
        }

        SelectedEntities = roots.ToArray();
        PrimalEditor_Avalonia.Common.Logger.Info(
            $"[Prefab] instantiated '{prefab.Name}' ({roots.Count} root(s)) from {info.FileName}");
        return roots[0];
    }

    /// <summary>
    /// A5 Array 批量布设：把选区沿轴深拷 count 份、逐份叠加 offset*i 的世界
    /// 位移（offset 取吸附步长时阵列天然落格）。子级随根保持相对变换。
    /// 整批一个 undo 事务，返回创建总数。
    /// </summary>
    public int ArrayDuplicateSelected(string axis, int count, float offset)
    {
        if (ActiveScene == null || count <= 0) return 0;
        var sources = SelectedEntities
            .Where(e => e.GetTransform() != null)
            .ToArray();
        if (sources.Length == 0) return 0;

        int created = 0;
        using (UndoRedo.BeginTransaction($"Array duplicate {sources.Length}×{count} along {axis}"))
        {
            foreach (var source in sources)
            {
                var documents = PrimalEditor_Avalonia.GameProject.Serializer.ToEntityDocuments(new[] { source });
                for (int i = 1; i <= count; ++i)
                {
                    var clones = PrimalEditor_Avalonia.GameProject.Serializer.InstantiateEntityDocuments(
                        documents, reassignGuids: true);
                    var clone = clones[0];
                    // 引擎无父子变换组合（transform 全世界系）——平移必须落到
                    // 克隆子树的每个实体，否则子级在场景里"掉队"。
                    OffsetSubtree(clone, axis, offset * i);
                    AddEntity(clone);
                    var action = new AddEntityAction(this, clone);
                    UndoRedo.Add(action);
                    action.Redo();
                    ++created;
                }
            }
        }
        PrimalEditor_Avalonia.Common.Logger.Info($"[Array] created {created} entities ({axis}, step {offset}m)");
        return created;
    }

    /// <summary>
    /// A6: 把 PCG 图输出点批量落成场景实体。positions 为 XYZ 三元组、
    /// transforms 为 {scaleX,scaleY,scaleZ,rotY} 四元组（弧度，与引擎直传
    /// 同单位）、meshSlots 为逐点 mesh 库索引（按 slotPaths 解析为注册
    /// 资产；解析失败的点只建 Transform）。整批一个 undo 事务；SyncEntity
    /// 逐实体推引擎（文档模型优先——不裸调引擎建第二事实源）。
    /// </summary>
    public int ApplyPcgPointsToScene(Editors.Pcg.PcgApplyPayload payload)
    {
        if (ActiveScene == null || payload.Positions.Length == 0) return 0;
        int pointCount = payload.Positions.Length / 3;

        // mesh 库索引 → 注册资产 GUID。slot 路径是工程相对路径（如
        // "Content/Props/Tree.model"）；cooked 链路的注册名是 .model.json，
        // 两种候选都查。
        var slotGuids = new AssetGuid?[payload.MeshSlotPaths.Count];
        for (int s = 0; s < payload.MeshSlotPaths.Count; ++s)
        {
            var rel = payload.MeshSlotPaths[s].Path.Replace('\\', '/');
            if (Project == null) break;
            var candidates = new[]
            {
                System.IO.Path.Combine(Project.Path, rel),
                System.IO.Path.Combine(Project.Path, System.IO.Path.ChangeExtension(rel, ".model.json")),
            };
            foreach (var abs in candidates)
            {
                var asset = AssetRegistry.Get(abs);
                if (asset != null)
                {
                    slotGuids[s] = new AssetGuid(asset.Guid);
                    break;
                }
            }
            if (slotGuids[s] == null)
                PrimalEditor_Avalonia.Common.Logger.Warning(
                    $"[PCG] mesh slot '{rel}' did not resolve to a registered asset — points will have no mesh");
        }

        int created = 0;
        using (UndoRedo.BeginTransaction($"PCG apply {pointCount} points"))
        {
            for (int i = 0; i < pointCount; ++i)
            {
                var entity = new GameEntity($"{payload.GraphName}_{i}");
                entity.Components.Add(new Transform
                {
                    PositionX = payload.Positions[i * 3],
                    PositionY = payload.Positions[i * 3 + 1],
                    PositionZ = payload.Positions[i * 3 + 2],
                });
                if (payload.Transforms.Length >= (i + 1) * 4)
                {
                    var tr = entity.GetTransform();
                    if (tr != null)
                    {
                        tr.ScaleX = payload.Transforms[i * 4];
                        tr.ScaleY = payload.Transforms[i * 4 + 1];
                        tr.ScaleZ = payload.Transforms[i * 4 + 2];
                        tr.RotationY = payload.Transforms[i * 4 + 3];
                    }
                }
                uint slot = i < payload.MeshSlots.Length ? payload.MeshSlots[i] : uint.MaxValue;
                if (slot < slotGuids.Length && slotGuids[slot] != null)
                    entity.Components.Add(new MeshRendererComponent { MeshGuid = slotGuids[slot]!.Value });

                AddEntity(entity);
                var action = new AddEntityAction(this, entity);
                UndoRedo.Add(action);
                action.Redo();
                if (NativeEngineBridge.IsLoaded) SceneSynchronizer.SyncEntity(entity);
                ++created;
            }
        }
        PrimalEditor_Avalonia.Common.Logger.Info($"[PCG] applied {created} entities to scene");
        return created;
    }

    private static void OffsetSubtree(GameEntity root, string axis, float delta)
    {
        void Walk(GameEntity entity)
        {
            if (entity.GetTransform() is { } t)
            {
                switch (axis)
                {
                    case "Y": t.PositionY += delta; break;
                    case "Z": t.PositionZ += delta; break;
                    default: t.PositionX += delta; break;
                }
            }
            foreach (var child in entity.Children) Walk(child);
        }
        Walk(root);
    }

    internal static (string? nativePath, string entityName) ResolveDropTarget(AssetInfo info)
    {
        const string modelSidecarSuffix = ".model.json";
        if (info.FullPath.EndsWith(modelSidecarSuffix, StringComparison.OrdinalIgnoreCase))
        {
            var basePath = info.FullPath[..^modelSidecarSuffix.Length];
            var cookedMesh = basePath + ".mesh";
            var sidecarEntityName = System.IO.Path.GetFileName(basePath);
            return System.IO.File.Exists(cookedMesh)
                ? (cookedMesh, sidecarEntityName)
                : (null, sidecarEntityName);
        }

        var ext = System.IO.Path.GetExtension(info.FullPath).ToLowerInvariant();
        var entityName = System.IO.Path.GetFileNameWithoutExtension(info.FullPath);
        switch (ext)
        {
            case ".fbx":
            case ".obj":
            case ".gltf":
            case ".glb":
            case ".mesh":
                return (info.FullPath, entityName);
            case ".model":
            {
                var cookedMesh = System.IO.Path.ChangeExtension(info.FullPath, ".mesh");
                if (!System.IO.File.Exists(cookedMesh)) return (null, entityName);
                return (cookedMesh, entityName);
            }
            default:
                return (null, entityName);
        }
    }

    /// <summary>
    /// A model imported through MeshImporterPlugin gets a sibling default
    /// material ("&lt;stem&gt;.material.json" with a graph referencing the
    /// copied textures). Assign it on drop so the placed entity renders
    /// textured — SceneRuntimeSynchronizer binds the albedo/normal/ORM
    /// triple from the first entry of MaterialGuids and falls back to the
    /// white default when the list is empty. No-op when the model has no
    /// sibling material (e.g. source files dropped without importing).
    /// </summary>
    internal static void TryAssignDefaultMaterial(MeshRendererComponent renderer, string modelAssetPath)
    {
        var fileName = System.IO.Path.GetFileName(modelAssetPath);
        const string modelSidecarSuffix = ".model.json";
        var stem = fileName.EndsWith(modelSidecarSuffix, StringComparison.OrdinalIgnoreCase)
            ? fileName[..^modelSidecarSuffix.Length]
            : System.IO.Path.GetFileNameWithoutExtension(fileName);
        if (string.IsNullOrEmpty(stem)) return;

        var materialPath = System.IO.Path.Combine(
            System.IO.Path.GetDirectoryName(modelAssetPath) ?? string.Empty, stem + ".material.json");
        var material = AssetRegistry.Get(materialPath);
        if (material == null || material.Guid == Guid.Empty) return;

        renderer.MaterialGuids.Add(new AssetGuid(material.Guid));
    }

    /// <summary>
    /// Re-import every entity in the active scene whose AssetPath is
    /// non-empty. Each successful import produces a fresh native
    /// mesh content id which is registered against the C# entity in
    /// <see cref="RuntimeEntityMap"/>. The editor's transform gizmo
    /// can then write back to the engine without the user having to
    /// re-drag the asset.
    /// </summary>
    [Obsolete("Reads the v1 AssetPath field; v2 scenes use MeshRendererComponent.MeshGuid and are now driven by SceneRuntimeSynchronizer. Retained only for migration tooling.")]
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
