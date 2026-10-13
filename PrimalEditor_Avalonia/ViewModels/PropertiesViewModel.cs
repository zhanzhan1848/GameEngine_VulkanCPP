using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.ViewModels;

public class PropertiesViewModel : ViewModelBase
{
    private MSEntity _msEntity = new();
    private string _mixedValueMessage = string.Empty;

    public bool HasSelection => _msEntity.SelectedEntities.Count > 0;
    public bool HasMultipleSelection => _msEntity.SelectedEntities.Count > 1;
    public bool HasMixedValues => !string.IsNullOrEmpty(_mixedValueMessage);

    public MSEntity MSEntity
    {
        get => _msEntity;
        private set
        {
            _msEntity = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(HasSelection));
            OnPropertyChanged(nameof(HasMultipleSelection));
        }
    }

    public string MixedValueMessage
    {
        get => _mixedValueMessage;
        set
        {
            if (_mixedValueMessage != value)
            {
                _mixedValueMessage = value;
                OnPropertyChanged(nameof(MixedValueMessage));
                OnPropertyChanged(nameof(HasMixedValues));
            }
        }
    }

    public MSTransform? Transform { get; private set; }

    private MSLight? _light;
    public MSLight? Light
    {
        get => _light;
        private set
        {
            if (_light != null) _light.PropertyChanged -= OnLightPropertyChanged;
            _light = value;
            if (_light != null) _light.PropertyChanged += OnLightPropertyChanged;
        }
    }

    public MSCamera? Camera { get; private set; }
    public MSMeshRenderer? MeshRenderer { get; private set; }

    // ---- 物理组件（M2-D）----
    private MSCollider? _collider;
    public MSCollider? Collider
    {
        get => _collider;
        private set
        {
            if (_collider != null) _collider.PropertyChanged -= OnComponentPropertyChanged;
            _collider = value;
            if (_collider != null) _collider.PropertyChanged += OnComponentPropertyChanged;
        }
    }

    private MSRigidBody? _rigidBody;
    public MSRigidBody? RigidBody
    {
        get => _rigidBody;
        private set
        {
            if (_rigidBody != null) _rigidBody.PropertyChanged -= OnComponentPropertyChanged;
            _rigidBody = value;
            if (_rigidBody != null) _rigidBody.PropertyChanged += OnComponentPropertyChanged;
        }
    }

    public bool HasColliderComponent => Collider != null;
    public bool HasRigidBodyComponent => RigidBody != null;

    // Inspector 绑定包装（ComboBox 用 SelectedIndex；nullable float 直绑）
    public System.Collections.Generic.IReadOnlyList<string> ColliderShapeOptions { get; } =
        new[] { "Box", "Sphere" };
    public int ColliderShapeIndex
    {
        get => Collider?.Shape ?? 0;
        set { if (Collider != null) Collider.Shape = value; }
    }
    public float? ColliderHalfX { get => Collider?.HalfX; set { if (Collider != null) Collider.HalfX = value; } }
    public float? ColliderHalfY { get => Collider?.HalfY; set { if (Collider != null) Collider.HalfY = value; } }
    public float? ColliderHalfZ { get => Collider?.HalfZ; set { if (Collider != null) Collider.HalfZ = value; } }
    public bool? ColliderIsTrigger { get => Collider?.IsTrigger; set { if (Collider != null) Collider.IsTrigger = value; } }
    public int? ColliderLayer { get => Collider?.Layer; set { if (Collider != null) Collider.Layer = value; } }

    public System.Collections.Generic.IReadOnlyList<string> RigidBodyModeOptions { get; } =
        new[] { "Static", "Dynamic", "Kinematic" };
    public int RigidBodyModeIndex
    {
        get => RigidBody?.Mode ?? 1;
        set { if (RigidBody != null) RigidBody.Mode = value; }
    }
    public float? RigidBodyMass { get => RigidBody?.Mass; set { if (RigidBody != null) RigidBody.Mass = value; } }
    public float? RigidBodyRestitution { get => RigidBody?.Restitution; set { if (RigidBody != null) RigidBody.Restitution = value; } }
    public float? RigidBodyFriction { get => RigidBody?.Friction; set { if (RigidBody != null) RigidBody.Friction = value; } }

    /// <summary>
    /// Component sections are visible only when every selected entity owns
    /// that component. This keeps multi-editing predictable: a component edit
    /// is never silently applied to just a subset of the selection.
    /// </summary>
    public bool HasLightComponent => Light != null;

    /// <summary>True when every selected entity carries an
    /// EnvironmentComponent — the inspector then shows the scene
    /// environment panel in place of light parameters.</summary>
    public bool HasEnvironmentComponent => _msEntity.SelectedEntities.Count > 0
        && _msEntity.SelectedEntities.All(e => e.GetComponent<EnvironmentComponent>() != null);
    public bool HasCameraComponent => Camera != null;
    public bool HasMeshRendererComponent => MeshRenderer != null;
    public bool HasMaterialOverrideComponent { get; private set; }

    // ---- 粒子发射器（plan §6.3 B4 P2）----
    private System.Collections.Generic.IReadOnlyList<PrimalEditor_Avalonia.Components.GameEntity> SingleSelection
        => _msEntity.SelectedEntities.Count == 1
            ? new[] { _msEntity.SelectedEntities[0] }
            : System.Array.Empty<PrimalEditor_Avalonia.Components.GameEntity>();

    public bool HasParticleComponent => SingleSelection.Count == 1 &&
        SingleSelection[0].GetComponent<PrimalEditor_Avalonia.Components.ParticleEmitterComponent>() != null;

    public PrimalEditor_Avalonia.Components.ParticleEmitterComponent? Particle
    {
        get
        {
            var c = SingleSelection.Count == 1
                ? SingleSelection[0].GetComponent<PrimalEditor_Avalonia.Components.ParticleEmitterComponent>()
                : null;
            ParticlePresetNameStatic = c?.PresetName;
            return c;
        }
    }

    // ---- Particle FX（PFX，N1 基础表单）----

    public bool HasParticleFXComponent => SingleSelection.Count == 1 &&
        SingleSelection[0].GetComponent<PrimalEditor_Avalonia.Components.ParticleFXComponent>() != null;

    public PrimalEditor_Avalonia.Components.ParticleFXComponent? ParticleFX
    {
        get
        {
            var c = SingleSelection.Count == 1
                ? SingleSelection[0].GetComponent<PrimalEditor_Avalonia.Components.ParticleFXComponent>()
                : null;
            ParticleFXInstanceStatic = c;
            ParticleFXAssetPathStatic = c?.AssetPath;
            return c;
        }
    }

    public string ParticleFXAssetPath
    {
        get => ParticleFX?.AssetPath ?? "";
        set
        {
            if (ParticleFX == null || ParticleFX.AssetPath == value) return;
            ParticleFX.AssetPath = value;
            MarkProjectDirtyCallback?.Invoke();
            // 换绑/清除资产 → 立即按组件当前状态重建该实体的原生发射器
            //（回调由 WorldEditorViewModel 注入，走 SceneRuntimeSynchronizer）
            PfxEmitterResyncCallback?.Invoke();
        }
    }

    /// <summary>Inspector 改动 PFX 资产绑定后请求同步器重建发射器（静态注入）。</summary>
    public static System.Action? PfxEmitterResyncCallback;

    public bool ParticleFXEnabled
    {
        get => ParticleFX?.IsEnabled ?? false;
        set
        {
            if (ParticleFX == null) return;
            ParticleFX.IsEnabled = value;
            MarkProjectDirtyCallback?.Invoke();
        }
    }

    /// <summary>同步 ExposedParams 到引擎发射器（遍历 rows，逐个调 PfxSetExposedParam）。</summary>
    public System.Windows.Input.ICommand PfxSyncParamsCommand { get; } =
        new PrimalEditor_Avalonia.Common.RelayCommand(_ =>
        {
            var pfx = ParticleFXInstanceStatic;
            if (pfx == null) return;
            var nativeId = SelectedEntityNativeIdStatic;
            if (nativeId == 0) return;

            foreach (var kv in pfx.ExposedParams)
            {
                var hash = PrimalEditor_Avalonia.Native.PfxHash.HashName(kv.Key);
                PrimalEditor_Avalonia.Native.NativeEngineBridge.PfxSetExposedParam(nativeId, hash, kv.Value);
            }
            Logger.Info($"[ParticleFX] synced {pfx.ExposedParams.Count} exposed params");
        });

    // 静态中转（RelayCommand 闭包无法捕获实例——编辑器单实例语义）
    internal static PrimalEditor_Avalonia.Components.ParticleFXComponent? ParticleFXInstanceStatic;
    internal static uint SelectedEntityNativeIdStatic;
    private static string? ParticleFXAssetPathStatic;

    /// <summary>Opens the selected entity's .pfx_emitter asset in the PFX
    /// module-stack editor window (material-editor style).</summary>
    public System.Windows.Input.ICommand PfxOpenEditorCommand { get; } =
        new PrimalEditor_Avalonia.Common.RelayCommand(_ =>
        {
            var path = ParticleFXAssetPathStatic;
            if (string.IsNullOrWhiteSpace(path))
            {
                PrimalEditor_Avalonia.Common.Logger.Info("[ParticleFX] open editor: entity has no asset path");
                return;
            }
            PfxOpenEditorCallbackStatic?.Invoke(path);
        });

    /// <summary>WorldEditorViewModel 注入：按资产路径打开 PFX 编辑器窗口。</summary>
    public System.Action<string>? PfxOpenEditorCallback
    {
        get => PfxOpenEditorCallbackStatic;
        set => PfxOpenEditorCallbackStatic = value;
    }

    private static System.Action<string>? PfxOpenEditorCallbackStatic;

    // ---- Lua 脚本（scripting S1 收官：Inspector 参数编辑段）----
    // 语义与 CLI script.set-param 一致：Parameters 是唯一事实源（文档模型），
    // 应用 = 快照旧值 → 写新字典 → UndoRedo → SyncEntity 销毁重建引擎实例
    //（begin_play 以新参数重放）→ MarkProjectDirty。

    public bool HasLuaScriptComponent => SingleSelection.Count == 1 &&
        SingleSelection[0].GetComponent<PrimalEditor_Avalonia.Components.LuaScriptComponent>() != null;

    public PrimalEditor_Avalonia.Components.LuaScriptComponent? LuaScript
    {
        get
        {
            LuaActivate(this);
            return SingleSelection.Count == 1
                ? SingleSelection[0].GetComponent<PrimalEditor_Avalonia.Components.LuaScriptComponent>()
                : null;
        }
    }

    /// <summary>资产显示名：注册表解析失败回退组件 ScriptName，再回退占位。</summary>
    public string LuaScriptAssetName
    {
        get
        {
            var script = LuaScript;
            if (script == null) return "";
            if (script.ScriptAssetGuid.Value != Guid.Empty)
            {
                var info = AssetRegistry.GetByGuid(script.ScriptAssetGuid.Value);
                if (!string.IsNullOrEmpty(info?.FileName)) return info.FileName;
            }
            return string.IsNullOrEmpty(script.ScriptName) ? "(未绑定)" : script.ScriptName;
        }
    }

    private System.Collections.ObjectModel.ObservableCollection<ScriptParamRow>? _luaParamRows;

    public System.Collections.ObjectModel.ObservableCollection<ScriptParamRow>? LuaParamRows
    {
        get
        {
            LuaActivate(this);
            if (_luaParamRows == null && LuaScript != null)
            {
                _luaParamRows = new System.Collections.ObjectModel.ObservableCollection<ScriptParamRow>();
                foreach (var kv in LuaScript.Parameters.OrderBy(kv => kv.Key, StringComparer.Ordinal))
                    _luaParamRows.Add(new ScriptParamRow(kv.Key, kv.Value));
            }
            return _luaParamRows;
        }
    }

    public void RefreshLuaParamRows()
    {
        _luaParamRows = null;
        OnPropertyChanged(nameof(LuaParamRows));
    }

    // 添加行输入缓冲
    private string _luaNewParamName = "";
    public string LuaNewParamName
    {
        get => _luaNewParamName;
        set { _luaNewParamName = value; OnPropertyChanged(); }
    }

    private string _luaNewParamValue = "";
    public string LuaNewParamValue
    {
        get => _luaNewParamValue;
        set { _luaNewParamValue = value; OnPropertyChanged(); }
    }

    /// <summary>WorldEditorViewModel 注入：参数应用后同步器销毁重建该实体的
    /// 引擎脚本实例（EnsureScript；等价 CLI 路径的 SceneSynchronizer.SyncEntity）。</summary>
    public static System.Action<PrimalEditor_Avalonia.Components.GameEntity>? ScriptResyncCallback;

    /// <summary>添加参数行（仅编辑缓冲；键规则同 script.set-param：非空、不含空格、
    /// 不与现有行重复）。非法输入记警告不动行集。</summary>
    public System.Windows.Input.ICommand LuaAddParamCommand { get; } =
        new PrimalEditor_Avalonia.Common.RelayCommand(_ =>
        {
            var vm = LuaActive;
            var rows = vm?._luaParamRows;
            if (vm == null || rows == null) return;
            var name = (vm.LuaNewParamName ?? "").Trim();
            if (name.Length == 0)
            {
                Logger.Warning("[Script] add param: name empty");
                return;
            }
            if (name.Contains(' '))
            {
                Logger.Warning($"[Script] add param: key '{name}' contains space — rejected");
                return;
            }
            if (rows.Any(r => r.Name == name))
            {
                Logger.Warning($"[Script] add param: key '{name}' already exists");
                return;
            }
            rows.Add(new ScriptParamRow(name, vm.LuaNewParamValue ?? ""));
            vm.LuaNewParamName = "";
            vm.LuaNewParamValue = "";
        });

    public void LuaRemoveParam(ScriptParamRow row) => _luaParamRows?.Remove(row);

    /// <summary>提交参数表到组件（文档模型）：跳过空键/含空格键并警告；成功才
    /// 记 Undo、触发重建与标脏。整表一次提交——逐行提交会在重建语义下产生
    /// 大量中间 begin_play 重放。</summary>
    public System.Windows.Input.ICommand LuaApplyParamsCommand { get; } =
        new PrimalEditor_Avalonia.Common.RelayCommand(_ => LuaActive?.ApplyLuaParams());

    private void ApplyLuaParams()
    {
        var script = LuaScript;
        var rows = _luaParamRows;
        if (script == null || rows == null) return;

        var next = new Dictionary<string, string>(StringComparer.Ordinal);
        var rejected = 0;
        foreach (var row in rows)
        {
            var name = (row.Name ?? "").Trim();
            if (name.Length == 0 || name.Contains(' ') || next.ContainsKey(name))
            {
                ++rejected;
                Logger.Warning($"[Script] apply params: key '{row.Name}' invalid (empty/space/duplicate) — skipped");
                continue;
            }
            next[name] = row.Value ?? "";
        }

        var entity = SingleSelection[0];
        var oldParams = new Dictionary<string, string>(script.Parameters, StringComparer.Ordinal);
        var changed = !oldParams.OrderBy(kv => kv.Key).SequenceEqual(
            next.OrderBy(kv => kv.Key), new KeyValuePairComparer());
        if (!changed)
        {
            Logger.Info("[Script] apply params: no change — skipped rebuild");
            return;
        }

        script.Parameters = next;
        var undoRedo = TryGetUndoRedo();
        if (undoRedo != null && !undoRedo.IsUndoingOrRedoing)
        {
            undoRedo.Add(new PrimalEditor_Avalonia.Common.DelegateAction("script.apply-params",
                () =>
                {
                    script.Parameters = new Dictionary<string, string>(oldParams, StringComparer.Ordinal);
                    ScriptResyncCallback?.Invoke(entity);
                },
                () =>
                {
                    script.Parameters = new Dictionary<string, string>(next, StringComparer.Ordinal);
                    ScriptResyncCallback?.Invoke(entity);
                }));
        }
        ScriptResyncCallback?.Invoke(entity);
        MarkProjectDirty();
        RefreshLuaParamRows();
        Logger.Info($"[Script] applied {next.Count} params ({rejected} rejected) to '{entity.Name}'");
    }

    // RelayCommand 闭包无法捕获实例——经静态中转（编辑器单实例语义，PFX 同款）：
    // Entities 绑定选中集时登记活动实例，命令经它取行集与输入缓冲。
    private static PropertiesViewModel? LuaActive;
    private static void LuaActivate(PropertiesViewModel vm) => LuaActive = vm;

    private sealed class KeyValuePairComparer : IEqualityComparer<KeyValuePair<string, string>>
    {
        public bool Equals(KeyValuePair<string, string> x, KeyValuePair<string, string> y) =>
            x.Key == y.Key && x.Value == y.Value;
        public int GetHashCode(KeyValuePair<string, string> obj) =>
            System.HashCode.Combine(obj.Key, obj.Value);
    }

    // ---- ExposedParams 行模型（DataGrid 绑定）----

    private System.Collections.ObjectModel.ObservableCollection<PfxExposedParamRow>? _pfxParamRows;

    public System.Collections.ObjectModel.ObservableCollection<PfxExposedParamRow>? PfxParamRows
    {
        get
        {
            if (_pfxParamRows == null && ParticleFX != null)
            {
                _pfxParamRows = new System.Collections.ObjectModel.ObservableCollection<PfxExposedParamRow>();
                foreach (var kv in ParticleFX.ExposedParams)
                    _pfxParamRows.Add(new PfxExposedParamRow { Name = kv.Key, Value = kv.Value });
            }
            return _pfxParamRows;
        }
    }

    public void RefreshPfxParamRows()
    {
        _pfxParamRows = null;
        OnPropertyChanged(nameof(PfxParamRows));
    }

    
    private System.Collections.Generic.IReadOnlyList<string>? _particlePresets;
    public System.Collections.Generic.IReadOnlyList<string> ParticlePresetOptions
        => _particlePresets ??= PrimalEditor_Avalonia.Native.NativeEngineBridge.ParticlePresetList()?
               .Split('\n') ?? new[] { "Fire", "Smoke", "Explosion", "Magic Sparkle", "Rain", "Snow", "Dust", "Bubbles" };

    /// <summary>B4 P4: drop the cached preset list so the next read re-queries
    /// the engine (after Save/Load adds a custom .particle.json).</summary>
    public void RefreshParticlePresetOptions()
    {
        _particlePresets = null;
        OnPropertyChanged(nameof(ParticlePresetOptions));
    }

    public string ParticlePresetName
    {
        get => Particle?.PresetName ?? "";
        set
        {
            if (Particle == null || Particle.PresetName == value) return;
            Particle.PresetName = value;
            RebuildParticleEmitter();
        }
    }

    public float ParticleSpawnRate
    {
        get => Particle?.SpawnRate ?? 0f;
        set
        {
            if (Particle == null) return;
            Particle.SpawnRate = value;
            if (SelectedEntityNativeId != 0)
                PrimalEditor_Avalonia.Native.NativeEngineBridge.EmitterSetSpawnRate(SelectedEntityNativeId, value);
            MarkProjectDirtyCallback?.Invoke();
        }
    }

    public bool ParticleEnabled
    {
        get => Particle?.IsEnabled ?? false;
        set
        {
            if (Particle == null) return;
            Particle.IsEnabled = value;
            if (SelectedEntityNativeId != 0)
                PrimalEditor_Avalonia.Native.NativeEngineBridge.EmitterSetSpawnRate(SelectedEntityNativeId,
                    value ? Particle.SpawnRate : 0f);
            MarkProjectDirtyCallback?.Invoke();
        }
    }

    private void RebuildParticleEmitter()
    {
        // 预设切换 = 拆掉旧发射器重建（PresetApplyToConfig 只在新实体创建路径
        // 被 Synchronizer 调用）。文档已标脏，保存后往返一致。
        PushParticlesRebuildCallback?.Invoke(_msEntity.SelectedEntities[0]);
        MarkProjectDirtyCallback?.Invoke();
    }

    /// <summary>WorldEditorViewModel 注入：移除旧发射器并按当前组件重建。</summary>
    public System.Action<PrimalEditor_Avalonia.Components.GameEntity>? PushParticlesRebuildCallback { get; set; }

    /// <summary>WorldEditorViewModel 注入：把当前预设存为项目内 .particle.json。
    /// 静态中转供 RelayCommand 闭包使用（编辑器单实例语义）。</summary>
    public System.Action<string>? ParticleSavePresetCallback
    {
        get => ParticleSavePresetCallbackStatic;
        set => ParticleSavePresetCallbackStatic = value;
    }

    public System.Windows.Input.ICommand ParticleSavePresetCommand { get; } =
        new PrimalEditor_Avalonia.Common.RelayCommand(_ =>
        {
            var name = EffectiveParticlePresetName();
            if (name != null) ParticleSavePresetCallbackStatic?.Invoke(name);
        });

    /// <summary>Opens the selected emitter's preset in the AssetEditorHost
    /// particle editor (material-editor style window). The callback exports
    /// the preset to .particle.json first when no file exists yet.</summary>
    public System.Windows.Input.ICommand ParticleOpenEditorCommand { get; } =
        new PrimalEditor_Avalonia.Common.RelayCommand(_ =>
        {
            var name = EffectiveParticlePresetName();
            if (name != null) ParticleOpenEditorCallbackStatic?.Invoke(name);
        });

    /// <summary>Preset name the commands should act on, or null when there is
    /// nothing to act on. An empty PresetName falls back to "fire", then the
    /// name is canonicalized against the engine's preset list — built-ins are
    /// stored capitalized ("Fire"/"Dust"/…) and preset_manager::get_preset is
    /// an exact-match lookup, so exporting a legacy lowercase "fire" fails
    /// otherwise (the observed "preset export failed: fire").</summary>
    private static string? EffectiveParticlePresetName()
    {
        if (ParticleOpenEditorCallbackStatic == null || ParticleSavePresetCallbackStatic == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning("[Particle] preset command: WorldEditor callback not wired");
            return null;
        }
        var name = string.IsNullOrEmpty(ParticlePresetNameStatic) ? "fire" : ParticlePresetNameStatic;
        var canonical = PrimalEditor_Avalonia.Native.NativeEngineBridge.ParticlePresetList()?
            .Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .FirstOrDefault(p => string.Equals(p, name, StringComparison.OrdinalIgnoreCase));
        if (canonical != null) name = canonical;
        PrimalEditor_Avalonia.Common.Logger.Info($"[Particle] preset command → '{name}'");
        return name;
    }

    /// <summary>WorldEditorViewModel 注入：打开（必要时先导出）预设文件编辑器。
    /// 静态中转供 RelayCommand 闭包使用（编辑器单实例语义）。</summary>
    public System.Action<string>? ParticleOpenEditorCallback
    {
        get => ParticleOpenEditorCallbackStatic;
        set => ParticleOpenEditorCallbackStatic = value;
    }

    // RelayCommand 闭包无法捕获实例——经静态中转（编辑器单实例语义）。
    private static string? ParticlePresetNameStatic;
    private static System.Action<string>? ParticleSavePresetCallbackStatic;
    private static System.Action<string>? ParticleOpenEditorCallbackStatic;


    /// <summary>"Look Through This Camera" or "Restore Editor Camera"
    /// depending on whether the viewport is currently rendering through
    /// this entity. Bound to the Properties panel button.</summary>
    public string LookThroughButtonLabel { get; private set; } = "Look Through This Camera";
    public string MainCameraButtonLabel { get; private set; } = "Set as Main Camera";

    /// <summary>Native entity id of the currently selected entity, or 0
    /// when nothing is selected. Set by <see cref="RefreshEntityContext"/>
    /// when the panel binds to a new selection.</summary>
    public uint SelectedEntityNativeId { get; private set; }

    public System.Collections.ObjectModel.ObservableCollection<MaterialOverrideViewModel> MaterialOverrides { get; } = new System.Collections.ObjectModel.ObservableCollection<MaterialOverrideViewModel>();
    public ObservableCollection<MeshMaterialSlotViewModel> MeshMaterials { get; } = new();
    public MeshAssetReferenceViewModel? MeshAsset { get; private set; }

    /// <summary>Called by the view after constructing this VM so it can
    /// route LookThroughCamera requests back to the viewport. Set via
    /// dependency injection (the WorldEditorView passes a delegate that
    /// forwards to the EngineViewport3D).</summary>
    public System.Action<uint>? LookThroughCameraCallback { get; set; }

    /// <summary>Called by the view after constructing this VM. Forwards to
    /// the viewport so the panel can ask "are we currently looking
    /// through the selected entity?".</summary>
    public System.Func<uint, bool>? IsLookingThroughCameraQuery { get; set; }
    public System.Action? SetMainCameraCallback { get; set; }
    public System.Func<bool>? IsMainCameraQuery { get; set; }

    // Injected by the owning WorldEditorViewModel. Inspector write-back must
    // not depend on finding a particular TopLevel/DataContext while docking
    // or window teardown is changing the visual tree.
    public System.Action<System.Collections.Generic.IEnumerable<GameEntity>>? PushTransformsToNativeCallback { get; set; }
    public System.Action<System.Collections.Generic.IEnumerable<GameEntity>>? PushMaterialsToNativeCallback { get; set; }
    public System.Action<System.Collections.Generic.IEnumerable<GameEntity>>? PushMeshesToNativeCallback { get; set; }
    public System.Action<AssetInfo>? RevealAssetCallback { get; set; }
    public System.Action? MarkProjectDirtyCallback { get; set; }
    public System.Func<UndoRedo?>? UndoRedoQuery { get; set; }

    public void RefreshEntityContext(uint selectedNativeId)
    {
        SelectedEntityNativeId = selectedNativeId;
        UpdateLookThroughLabel();
        UpdateMainCameraLabel();
    }

    private void UpdateLookThroughLabel()
    {
        if (SelectedEntityNativeId == 0 || IsLookingThroughCameraQuery == null)
        {
            LookThroughButtonLabel = "Look Through This Camera";
            OnPropertyChanged(nameof(LookThroughButtonLabel));
            return;
        }
        LookThroughButtonLabel = IsLookingThroughCameraQuery(SelectedEntityNativeId)
            ? "Restore Editor Camera"
            : "Look Through This Camera";
        OnPropertyChanged(nameof(LookThroughButtonLabel));
    }

    /// <summary>Called from the view's button click handler. Toggles the
    /// viewport's "look through this entity" state. If already looking
    /// through this entity, switches back to the editor camera.</summary>
    public void OnLookThroughCameraClicked()
    {
        if (SelectedEntityNativeId == 0 || LookThroughCameraCallback == null) return;
        if (IsLookingThroughCameraQuery != null
            && IsLookingThroughCameraQuery(SelectedEntityNativeId))
        {
            // Already looking through this one → switch back to editor.
            LookThroughCameraCallback(0u);
        }
        else
        {
            LookThroughCameraCallback(SelectedEntityNativeId);
        }
        UpdateLookThroughLabel();
    }

    /// <summary>Called from the view whenever the viewport's look-through
    /// state changes externally (e.g. another panel or shortcut). Re-runs
    /// the label refresh so the button text matches reality.</summary>
    public void NotifyLookThroughChanged()
    {
        UpdateLookThroughLabel();
    }

    public void OnSetMainCameraClicked()
    {
        if (SelectedEntityNativeId == 0) return;
        SetMainCameraCallback?.Invoke();
        UpdateMainCameraLabel();
    }

    public void NotifyMainCameraChanged() => UpdateMainCameraLabel();

    private void UpdateMainCameraLabel()
    {
        MainCameraButtonLabel = IsMainCameraQuery?.Invoke() == true
            ? "Main Camera"
            : "Set as Main Camera";
        OnPropertyChanged(nameof(MainCameraButtonLabel));
    }

    /// <summary>Stable list the Light.Type ComboBox binds against. Exposed
    /// here (not on MSLight) so the binding source survives across
    /// selection changes — MSLight is rebuilt every time the user picks
/// <summary>Stable list the Light.Type ComboBox binds against. Exposed
    /// here (not on MSLight) so the binding source survives across
    /// selection changes — MSLight is rebuilt every time the user picks
    /// a new entity, which would invalidate any ItemsSource bound to a
    /// view-model property on it.</summary>
    public IReadOnlyList<LightType> LightTypeOptions { get; } = new[] { LightType.Directional, LightType.Point, LightType.Spot };

    public GameEntity[] Entities
    {
        set
        {
            var ms = new MSEntity();
            LuaActivate(this);
            if (value != null)
                foreach (var e in value) ms.SelectedEntities.Add(e);
            MSEntity = ms;

            if (ms.SelectedEntities.Count == 0)
            {
                Transform = null;
                Light = null;
                Camera = null;
                MeshRenderer = null;
                Collider = null;
                RigidBody = null;
                MeshAsset = null;
                HasMaterialOverrideComponent = false;
                MaterialOverrides.Clear();
                MeshMaterials.Clear();
            }
            else
            {
                Transform = new MSTransform(ms);
                Light = HasCommonComponent<LightComponent>(ms) ? new MSLight(ms) : null;
                Camera = HasCommonComponent<CameraComponent>(ms) ? new MSCamera(ms) : null;
                MeshRenderer = HasCommonComponent<MeshRendererComponent>(ms) ? new MSMeshRenderer(ms) : null;
                Collider = HasCommonComponent<ColliderComponent>(ms) ? new MSCollider(ms) : null;
                RigidBody = HasCommonComponent<RigidBodyComponent>(ms) ? new MSRigidBody(ms) : null;
                RebuildMeshAsset(ms.SelectedEntities);
                HasMaterialOverrideComponent = HasCommonComponent<MaterialOverrideComponent>(ms);
                RebuildMaterialOverrides(ms.SelectedEntities);
                RebuildMeshMaterials(ms.SelectedEntities);
            }
            RefreshLuaParamRows();
            UpdateFromEntities();
        }
    }

    private static bool HasCommonComponent<T>(MSEntity entities) where T : Component =>
        entities.SelectedEntities.Count > 0
        && entities.SelectedEntities.All(entity => entity.GetComponent<T>() != null);

    private void RebuildMaterialOverrides(System.Collections.Generic.IList<Components.GameEntity> entities)
    {
        MaterialOverrides.Clear();
        if (entities.Count == 0) return;
        var first = entities[0];
        var firstOverride = first.GetComponent<Components.MaterialOverrideComponent>();
        if (firstOverride == null) return;
        for (int i = 0; i < firstOverride.Slots.Count; i++)
        {
            MaterialOverrides.Add(new MaterialOverrideViewModel(firstOverride.Slots[i], i));
        }
    }

    private void RebuildMeshMaterials(System.Collections.Generic.IList<GameEntity> entities)
    {
        MeshMaterials.Clear();
        var renderer = entities.FirstOrDefault()?.GetComponent<MeshRendererComponent>();
        if (renderer == null) return;

        // Every renderable mesh has at least one editable material element,
        // even before a material has been assigned. Imported multi-submesh
        // assets retain all of their existing persisted slots.
        var count = System.Math.Max(1, renderer.MaterialGuids.Count);
        for (var index = 0; index < count; index++)
        {
            var guid = index < renderer.MaterialGuids.Count
                ? renderer.MaterialGuids[index]
                : new AssetGuid(Guid.Empty);
            MeshMaterials.Add(new MeshMaterialSlotViewModel(index, guid));
        }
    }

    private void RebuildMeshAsset(System.Collections.Generic.IList<GameEntity> entities)
    {
        var renderer = entities.FirstOrDefault()?.GetComponent<MeshRendererComponent>();
        MeshAsset = renderer == null
            ? null
            : new MeshAssetReferenceViewModel(renderer.MeshGuid, renderer.BuiltinMesh);
        OnPropertyChanged(nameof(MeshAsset));
    }

    public bool AssignMeshAsset(AssetInfo asset)
    {
        if (asset == null || _msEntity.SelectedEntities.Count == 0) return false;
        var type = asset.Type != AssetType.Unknown
            ? asset.Type
            : ContentHelper.GetAssetType(asset.FullPath);
        if (type != AssetType.Mesh && type != AssetType.Model) return false;

        if (asset.Guid == Guid.Empty && !string.IsNullOrWhiteSpace(asset.FullPath))
        {
            AssetRegistry.Register(asset.FullPath);
            asset = AssetRegistry.Get(asset.FullPath) ?? asset;
        }
        if (asset.Guid == Guid.Empty) return false;

        var next = new AssetGuid(asset.Guid);
        var changed = new List<GameEntity>();
        foreach (var entity in _msEntity.SelectedEntities)
        {
            var renderer = entity.GetComponent<MeshRendererComponent>();
            if (renderer == null) continue;
            if (renderer.MeshGuid == next && renderer.BuiltinMesh == BuiltinMeshKind.None) continue;
            renderer.MeshGuid = next;
            renderer.BuiltinMesh = BuiltinMeshKind.None;
            changed.Add(entity);
        }
        if (changed.Count == 0) return false;

        RebuildMeshAsset(_msEntity.SelectedEntities);
        MeshRenderer?.Refresh();
        MarkProjectDirty();
        PushMeshesToNativeCallback?.Invoke(changed);
        return true;
    }

    public bool ClearMeshAsset()
    {
        var changed = new List<GameEntity>();
        foreach (var entity in _msEntity.SelectedEntities)
        {
            var renderer = entity.GetComponent<MeshRendererComponent>();
            if (renderer == null) continue;
            if (renderer.MeshGuid.Value == Guid.Empty && renderer.BuiltinMesh == BuiltinMeshKind.None) continue;
            renderer.MeshGuid = new AssetGuid(Guid.Empty);
            renderer.BuiltinMesh = BuiltinMeshKind.None;
            changed.Add(entity);
        }
        if (changed.Count == 0) return false;

        RebuildMeshAsset(_msEntity.SelectedEntities);
        MeshRenderer?.Refresh();
        MarkProjectDirty();
        PushMeshesToNativeCallback?.Invoke(changed);
        return true;
    }

    public void RevealMeshAsset()
    {
        if (MeshAsset?.Asset is { } asset) RevealAssetCallback?.Invoke(asset);
    }

    public bool AssignMeshMaterial(int slotIndex, AssetInfo asset)
    {
        if (slotIndex < 0 || asset == null || _msEntity.SelectedEntities.Count == 0) return false;
        var type = asset.Type != AssetType.Unknown
            ? asset.Type
            : ContentHelper.GetAssetType(asset.FullPath);
        if (type != AssetType.Material) return false;

        if (asset.Guid == Guid.Empty && !string.IsNullOrWhiteSpace(asset.FullPath))
        {
            AssetRegistry.Register(asset.FullPath);
            asset = AssetRegistry.Get(asset.FullPath) ?? asset;
        }
        if (asset.Guid == Guid.Empty) return false;

        var changedEntities = new List<GameEntity>();
        foreach (var entity in _msEntity.SelectedEntities)
        {
            var renderer = entity.GetComponent<MeshRendererComponent>();
            if (renderer == null) continue;
            while (renderer.MaterialGuids.Count <= slotIndex)
                renderer.MaterialGuids.Add(new AssetGuid(Guid.Empty));
            var next = new AssetGuid(asset.Guid);
            if (renderer.MaterialGuids[slotIndex] == next) continue;
            renderer.MaterialGuids[slotIndex] = next;
            changedEntities.Add(entity);
        }

        if (changedEntities.Count == 0) return false;
        RebuildMeshMaterials(_msEntity.SelectedEntities);
        MarkProjectDirty();
        PushMaterialsToNativeCallback?.Invoke(changedEntities);
        return true;
    }

    public string EntityName
    {
        get => _msEntity.Name;
        set
        {
            if (_msEntity.SelectedEntities.Count == 1 && _msEntity.SelectedEntities[0].Name != value)
            {
                _msEntity.SelectedEntities[0].Name = value;
                OnPropertyChanged();
            }
        }
    }

    public bool IsEnabled
    {
        get => _msEntity.IsEnabled;
        set { _msEntity.IsEnabled = value; OnPropertyChanged(); }
    }

    public float? PosX   { get => Transform?.PosX;   set { if (Transform != null) { RecordAndApply(nameof(PosX),   () => Transform.PosX   = value); } } }
    public float? PosY   { get => Transform?.PosY;   set { if (Transform != null) { RecordAndApply(nameof(PosY),   () => Transform.PosY   = value); } } }
    public float? PosZ   { get => Transform?.PosZ;   set { if (Transform != null) { RecordAndApply(nameof(PosZ),   () => Transform.PosZ   = value); } } }
    public float? RotX   { get => Transform?.RotX;   set { if (Transform != null) { RecordAndApply(nameof(RotX),   () => Transform.RotX   = value); } } }
    public float? RotY   { get => Transform?.RotY;   set { if (Transform != null) { RecordAndApply(nameof(RotY),   () => Transform.RotY   = value); } } }
    public float? RotZ   { get => Transform?.RotZ;   set { if (Transform != null) { RecordAndApply(nameof(RotZ),   () => Transform.RotZ   = value); } } }
    public float? ScaleX { get => Transform?.ScaleX; set { if (Transform != null) { RecordAndApply(nameof(ScaleX), () => Transform.ScaleX = value); } } }
    public float? ScaleY { get => Transform?.ScaleY; set { if (Transform != null) { RecordAndApply(nameof(ScaleY), () => Transform.ScaleY = value); } } }
    public float? ScaleZ { get => Transform?.ScaleZ; set { if (Transform != null) { RecordAndApply(nameof(ScaleZ), () => Transform.ScaleZ = value); } } }

    private void RecordAndApply(string propertyName, Action applyChange)
    {
        // Capture snapshot BEFORE applying the change so the old state is the
        // pre-change transform, and the new state is the post-change one.
        var undoRedo = TryGetUndoRedo();
        var oldDataList = SnapshotSelected();
        applyChange();
        var newDataList = SnapshotSelected();
        OnPropertyChanged(propertyName);

        var changed = oldDataList.Count == 0 || HasAnyChange(oldDataList, newDataList);
        if (changed && undoRedo != null && !undoRedo.IsUndoingOrRedoing)
        {
            undoRedo.Add(new MultiEntityTransformAction(oldDataList, newDataList));
        }
        if (changed) MarkProjectDirty();

        // Mirror the C# transform into the native engine so the render
        // surface reflects the new value immediately. Without this the
        // gizmo would update on the next SceneSynced() tick (which only
        // fires on ActiveScene changes), leaving the viewport showing
        // the stale position until the user clicks away and back.
        PushTransformsToNative();
    }

    private void PushTransformsToNative()
    {
        var selected = _msEntity.SelectedEntities;
        if (selected.Count == 0) return;
        if (PushTransformsToNativeCallback != null)
        {
            PushTransformsToNativeCallback(selected);
            return;
        }
        if (!NativeEngineBridge.IsLoaded) return;
        // Walk the same MainWindow data-context chain as TryGetUndoRedo
        // so we hit the same WorldEditorViewModel instance the rest of
        // the editor talks to. There's no cross-view-model service
        // locator in the editor yet, so this duplicated lookup is the
        // simplest reliable path.
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        var worldEditor = lifetime?.MainWindow?.DataContext is MainWindowViewModel mainVm
            ? mainVm.WorldEditor
            : null;
        if (worldEditor == null) return;
        foreach (var entity in selected)
        {
            var t = entity.GetTransform();
            if (t == null) continue;
            var nativeId = worldEditor.GetNativeEntityId(entity);
            if (nativeId == 0) continue;
            float[] pos = { t.PositionX, t.PositionY, t.PositionZ };
            float[] rot = { t.RotationX, t.RotationY, t.RotationZ };
            float[] scl = { t.ScaleX, t.ScaleY, t.ScaleZ };
            NativeEngineBridge.SetEntityTransform(nativeId, pos, rot, scl);
        }
    }

    // Live-push light edits to the engine so the viewport reflects color /
    // intensity / type / range changes without a scene resync. Same native
    // lookup pattern as PushTransformsToNative. Refresh()-driven events
    // (selection changes) re-push identical state, which is harmless.
    private void OnLightPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        PushLightsToNative();
    }

    // 物理组件（M2-D）：inspector 编辑 → Remove+Add 重物化原生 body
    //（EngineDLL 的 Add* 为一次性创建语义；M3 play 态机引入后可改为参数更新）
    private void OnComponentPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        PushPhysicsComponentsToNative();
    }

    private void PushPhysicsComponentsToNative()
    {
        var selected = _msEntity.SelectedEntities;
        if (selected.Count == 0) return;
        if (!NativeEngineBridge.IsLoaded) return;
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        var worldEditor = lifetime?.MainWindow?.DataContext is MainWindowViewModel mainVm
            ? mainVm.WorldEditor
            : null;
        if (worldEditor == null) return;

        foreach (var entity in selected)
        {
            var nativeId = worldEditor.GetNativeEntityId(entity);
            if (nativeId == 0) continue;

            var collider = entity.GetComponent<ColliderComponent>();
            if (collider != null && NativeEngineBridge.EntityHasCollider(nativeId))
            {
                NativeEngineBridge.RemoveEntityCollider(nativeId);
                NativeEngineBridge.AddEntityCollider(nativeId,
                    (uint)collider.Shape,
                    new[] { collider.HalfX, collider.HalfY, collider.HalfZ },
                    new[] { 0f, 0f, 0f },
                    collider.IsTrigger,
                    (uint)collider.Layer);
            }

            var rigidBody = entity.GetComponent<RigidBodyComponent>();
            if (rigidBody != null && NativeEngineBridge.EntityHasRigidBody(nativeId))
            {
                NativeEngineBridge.RemoveEntityRigidBody(nativeId);
                NativeEngineBridge.AddEntityRigidBody(nativeId,
                    (uint)rigidBody.Mode,
                    rigidBody.Mass,
                    rigidBody.Restitution,
                    rigidBody.Friction,
                    new[] { rigidBody.VelocityX, rigidBody.VelocityY, rigidBody.VelocityZ });
            }
        }
    }

    // ---- Scene environment (HDR sky + IBL), surfaced in the LIGHT section
    // of the inspector so the sun light's env map is editable in place.
    // Values delegate to the active scene's SceneEnvironmentViewModel; the
    // native push happens in its PushToNative (PipelineSetEnvironmentMap).
    /// <summary>
    /// The environment VM owned by the same WorldEditor as this inspector.
    /// Injected by WorldEditorViewModel so HDR edits do not depend on finding
    /// a particular top-level Avalonia window or docking configuration.
    /// </summary>
    public SceneEnvironmentViewModel? SceneEnvironment { get; set; }

    /// <summary>Scene environment settings VM — the data source for the
    /// ENVIRONMENT LIGHT inspector section. Stable singleton once the main
    /// window exists.</summary>
    public SceneEnvironmentViewModel? EnvironmentVM => ResolveEnvironmentViewModel();

    private SceneEnvironmentViewModel? ResolveEnvironmentViewModel()
    {
        if (SceneEnvironment != null) return SceneEnvironment;
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        return lifetime?.MainWindow?.DataContext is MainWindowViewModel mainVm
            ? mainVm.WorldEditor.SceneEnvironmentViewModel
            : null;
    }

    private string? _envMapPath;
    public string? EnvMapPath
    {
        get
        {
            var value = ResolveEnvironmentViewModel()?.EnvMapPath ?? _envMapPath;
            // Empty → null so the drop-slot watermark (TargetNullValue) shows.
            return string.IsNullOrEmpty(value) ? null : value;
        }
        set
        {
            _envMapPath = value;
            if (value != null && ResolveEnvironmentViewModel() is { } env)
            {
                env.EnvMapPath = value;
            }
            // Notify after updating the delegated VM. The binding getter
            // reads that VM, so firing first made a successful drop re-read
            // the old empty value and look as if the drop had been rejected.
            OnPropertyChanged(nameof(EnvMapPath));
        }
    }

    private bool _skyboxEnabled;
    public bool SkyboxEnabled
    {
        get => ResolveEnvironmentViewModel()?.SkyboxEnabled ?? _skyboxEnabled;
        set
        {
            _skyboxEnabled = value;
            if (ResolveEnvironmentViewModel() is { } env)
            {
                env.SkyboxEnabled = value;
            }
            OnPropertyChanged(nameof(SkyboxEnabled));
        }
    }

    private void PushLightsToNative()
    {
        var selected = _msEntity.SelectedEntities;
        if (selected.Count == 0 || Light == null) return;
        if (!NativeEngineBridge.IsLoaded) return;
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        var worldEditor = lifetime?.MainWindow?.DataContext is MainWindowViewModel mainVm
            ? mainVm.WorldEditor
            : null;
        if (worldEditor == null) return;

        foreach (var entity in selected)
        {
            var light = entity.GetComponent<LightComponent>();
            if (light == null) continue;
            var nativeId = worldEditor.GetNativeEntityId(entity);
            if (nativeId == 0) continue;
            NativeEngineBridge.SetEntityLight(nativeId,
                (uint)light.LightType,
                light.Intensity,
                new[] { light.ColorR, light.ColorG, light.ColorB },
                light.Range,
                light.UmbraAngle,
                light.PenumbraAngle,
                light.IsEnabled);
        }
    }

    private void MarkProjectDirty()
    {
        if (MarkProjectDirtyCallback != null)
        {
            MarkProjectDirtyCallback();
            return;
        }
        if (Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            && desktop.MainWindow?.DataContext is MainWindowViewModel mainVm
            && mainVm.Project != null)
        {
            mainVm.Project.MarkDirty();
            mainVm.Project.ActiveScene?.MarkDirty();
        }
    }

    private List<(GameEntity entity, TransformData data)> SnapshotSelected()
    {
        var list = new List<(GameEntity entity, TransformData data)>();
        foreach (var e in _msEntity.SelectedEntities)
        {
            var t = e.GetTransform();
            if (t == null) continue;
            list.Add((e, TransformData.FromTransform(t)));
        }
        return list;
    }

    private static bool HasAnyChange(
        List<(GameEntity entity, TransformData data)> oldList,
        List<(GameEntity entity, TransformData data)> newList)
    {
        if (oldList.Count != newList.Count) return true;
        for (int i = 0; i < oldList.Count; ++i)
        {
            if (!TransformDataEquals(oldList[i].data, newList[i].data)) return true;
        }
        return false;
    }

    private static bool TransformDataEquals(in TransformData a, in TransformData b)
    {
        const float Eps = 1e-4f;
        return System.Math.Abs(a.PositionX - b.PositionX) < Eps
            && System.Math.Abs(a.PositionY - b.PositionY) < Eps
            && System.Math.Abs(a.PositionZ - b.PositionZ) < Eps
            && System.Math.Abs(a.RotationX - b.RotationX) < Eps
            && System.Math.Abs(a.RotationY - b.RotationY) < Eps
            && System.Math.Abs(a.RotationZ - b.RotationZ) < Eps
            && System.Math.Abs(a.ScaleX - b.ScaleX) < Eps
            && System.Math.Abs(a.ScaleY - b.ScaleY) < Eps
            && System.Math.Abs(a.ScaleZ - b.ScaleZ) < Eps;
    }

    private UndoRedo? TryGetUndoRedo()
    {
        if (UndoRedoQuery != null) return UndoRedoQuery();
        if (Avalonia.Application.Current?.ApplicationLifetime is
            Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            && desktop.MainWindow?.DataContext is MainWindowViewModel mainVm)
        {
            return mainVm.WorldEditor.UndoRedo;
        }
        return null;
    }

    /// <summary>
    /// Re-fire PropertyChanged for the 9 transform properties so the
    /// inspector TextBoxes re-read from the MSTransform wrapper.
    /// Called by the gizmo drag handler after it mutates the
    /// underlying Transform component in place.
    /// </summary>
    public void RefreshFromTransform(Components.Transform t)
    {
        if (_msEntity.SelectedEntities.Count != 1) return;
        if (!ReferenceEquals(_msEntity.SelectedEntities[0].GetTransform(), t)) return;
        // Re-pull the backing values from the underlying Transform so
        // the panel reflects the new state. Without this the panel
        // shows the pre-gizmo values until the user re-selects the
        // entity (which rebuilds MSTransform and incidentally refreshes).
        Transform?.Refresh();
        NotifyTransformPropertiesChanged();
    }

    private void NotifyTransformPropertiesChanged()
    {
        // PropertiesView binds to these forwarding properties rather than to
        // MSTransform directly. Refreshing MSTransform alone therefore cannot
        // make Avalonia re-read the values after an external mutation such as
        // a gizmo drag.
        OnPropertyChanged(nameof(PosX));   OnPropertyChanged(nameof(PosY));   OnPropertyChanged(nameof(PosZ));
        OnPropertyChanged(nameof(RotX));   OnPropertyChanged(nameof(RotY));   OnPropertyChanged(nameof(RotZ));
        OnPropertyChanged(nameof(ScaleX)); OnPropertyChanged(nameof(ScaleY)); OnPropertyChanged(nameof(ScaleZ));
    }

    private void UpdateFromEntities()
    {
        OnPropertyChanged(nameof(EntityName));
        OnPropertyChanged(nameof(IsEnabled));
        NotifyTransformPropertiesChanged();

        Light?.Refresh();
        Camera?.Refresh();
        MeshRenderer?.Refresh();
        OnPropertyChanged(nameof(Light));
        OnPropertyChanged(nameof(Camera));
        OnPropertyChanged(nameof(MeshRenderer));
        OnPropertyChanged(nameof(MeshAsset));
        OnPropertyChanged(nameof(HasLightComponent));
        OnPropertyChanged(nameof(HasColliderComponent));
        OnPropertyChanged(nameof(HasRigidBodyComponent));
        OnPropertyChanged(nameof(HasEnvironmentComponent));
        OnPropertyChanged(nameof(EnvironmentVM));
        OnPropertyChanged(nameof(HasCameraComponent));
        OnPropertyChanged(nameof(HasMeshRendererComponent));
        OnPropertyChanged(nameof(HasMaterialOverrideComponent));
        OnPropertyChanged(nameof(HasParticleComponent));
        OnPropertyChanged(nameof(Particle));
        // scripting S1 收官：Lua 区段可见性/资产名/参数行随选中集刷新。
        OnPropertyChanged(nameof(HasLuaScriptComponent));
        OnPropertyChanged(nameof(LuaScriptAssetName));
        OnPropertyChanged(nameof(LuaParamRows));
        // Section visibility alone is not enough: without these the Preset
        // combo / Spawn Rate / Enabled keep the values they had before the
        // emitter existed (empty / 0 / unchecked) after re-selection.
        OnPropertyChanged(nameof(ParticlePresetName));
        OnPropertyChanged(nameof(ParticleSpawnRate));
        OnPropertyChanged(nameof(ParticleEnabled));

        MixedValueMessage = _msEntity.SelectedEntities.Count > 1
            ? "Multiple entities selected - showing first value or '-' for mixed"
            : string.Empty;
    }
}
