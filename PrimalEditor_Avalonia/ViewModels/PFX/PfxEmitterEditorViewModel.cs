using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Linq;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors;
using PrimalEditor_Avalonia.Editors.Particle;
using PrimalEditor_Avalonia.Models.PFX;
using PrimalEditor_Avalonia.Services;
using NativeEngineBridge = PrimalEditor_Avalonia.Native.NativeEngineBridge;

namespace PrimalEditor_Avalonia.ViewModels.PFX;

/// <summary>
/// One parameter of a module instance, typed by the engine schema
/// (PfxGetEmitterSchema): f32/u32 → numeric box, v2/v3/v4 → component boxes,
/// bool → checkbox, curve → embedded over-life curve editor, enum/param_ref →
/// free text with the schema note as guidance. Accessors mutate the row and
/// report through <paramref name="onChanged"/> so the editor marks itself
/// dirty and coalesces an undo snapshot.
/// </summary>
public sealed class PfxParamRow : INotifyPropertyChanged
{
    public enum ParamKind { Numeric, Vector, Bool, Enum, Curve, Text, Asset }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Fire([System.Runtime.CompilerServices.CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new(name));

    private Action _onChanged;

    /// <summary>True while the editor populates the row from the document —
    /// suppresses edit notifications so loading never marks the asset dirty.</summary>
    public bool Initializing { get; set; }

    public string Name { get; }
    public string Type { get; }
    public string Note { get; }
    public ParamKind Kind { get; }

    /// <summary>enum 参数的可选项（schema note 按管道分隔，如 "fixed|shape_normal"）。</summary>
    public string[] EnumOptions { get; set; } = Array.Empty<string>();

    /// <summary>所属模块名（曲线槽位映射/调试显示用）。</summary>
    public string ModuleName { get; init; } = "";

    /// <summary>曲线参数在共享 EmitterCurveEditor 上的值域槽位。</summary>
    public ParticleCurveSlot CurveSlot { get; init; } = ParticleCurveSlot.Alpha;

    public bool IsNumeric => Kind == ParamKind.Numeric;
    public bool IsVector => Kind == ParamKind.Vector;
    public bool IsBool => Kind == ParamKind.Bool;
    public bool IsCurve => Kind == ParamKind.Curve;
    public bool IsEnum => Kind == ParamKind.Enum;
    public bool IsText => Kind == ParamKind.Text;
    public bool IsAsset => Kind == ParamKind.Asset;
    /// <summary>P4.2:asset 类型参数的可选项(CWD 相对路径,即存储格式本身)。</summary>
    public string[] AssetOptions { get; set; } = Array.Empty<string>();

    // scalar / free text (enum + param_ref: schema carries no option lists)
    private string _value = "0";
    public string Value
    {
        get => _value;
        set { _value = value; if (!Initializing) NotifyEdited(); }
    }

    // vector components (v2 → X,Y; v3 → X,Y,Z; v4 → X..W)
    private string _x = "0", _y = "0", _z = "0", _w = "0";
    public string X { get => _x; set { _x = value; if (!Initializing) NotifyEdited(); } }
    public string Y { get => _y; set { _y = value; if (!Initializing) NotifyEdited(); } }
    public string Z { get => _z; set { _z = value; if (!Initializing) NotifyEdited(); } }
    public string W { get => _w; set { _w = value; if (!Initializing) NotifyEdited(); } }
    public int ComponentCount { get; set; } = 3;
    public bool ShowX => IsVector && ComponentCount > 0;
    public bool ShowY => IsVector && ComponentCount > 1;
    public bool ShowZ => IsVector && ComponentCount > 2;
    public bool ShowW => IsVector && ComponentCount > 3;

    private bool _boolValue;
    public bool BoolValue
    {
        get => _boolValue;
        set { _boolValue = value; if (!Initializing) NotifyEdited(); }
    }

    /// <summary>Curve payload for the embedded editor (null = empty curve).</summary>
    public ParticleCurveData? Curve { get; set; }

    /// <summary>Late-bound edit hook (the row is created before its owner).</summary>
    public void SetChangedCallback(Action onChanged) => _onChanged = onChanged ?? (() => { });

    public PfxParamRow(string name, string type, string note, ParamKind kind, Action onChanged)
    {
        Name = name;
        Type = type;
        Note = note;
        Kind = kind;
        _onChanged = onChanged;
    }

    public void NotifyEdited() => _onChanged();
    public void FireAll() { Fire(nameof(Value)); Fire(nameof(BoolValue)); Fire(nameof(Curve)); }
}

/// <summary>One module instance in the emitter's stage stack.</summary>
public sealed class PfxStackModuleRow : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    private void Fire([System.Runtime.CompilerServices.CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new(name));

    public string Stage { get; init; } = "update";
    public string Name { get => _name; set { _name = value; Fire(nameof(Name)); } }
    private string _name = "";

    public string StageTitle => Stage.ToUpperInvariant();

    public ObservableCollection<PfxParamRow> Params { get; } = new();

    public PfxStackModuleRow(string stage, string name) { Stage = stage; _name = name; }
}

/// <summary>
/// Asset-editor VM for .pfx_emitter programs (AssetType.PfxEmitter), hosted by
/// AssetEditorHost like the material editor. Editing paradigm is the PFX
/// module stack — the engine's programmable particle model: spawn/init/update
/// stages, schema-driven module parameters, over-life curves embedded, hot
/// preview into any live scene entity using this asset.
/// </summary>
public sealed class PfxEmitterEditorViewModel : AssetEditorViewModel
{
    private string _json = "{}";

    public ObservableCollection<PfxStackModuleRow> AllModules { get; } = new();

    public PfxSchema? Schema { get; private set; }

    public IReadOnlyList<string> StageNames => _stageNames;
    private readonly IReadOnlyList<string> _stageNames = new[] { "spawn", "init", "update" };

    private string _selectedStage = "spawn";
    public string SelectedStage
    {
        get => _selectedStage;
        set
        {
            _selectedStage = string.IsNullOrWhiteSpace(value) ? "spawn" : value;
            Fire(nameof(SelectedStage));
            Fire(nameof(VisibleStackModules));
            Fire(nameof(PaletteModules));
        }
    }

    public System.Collections.IEnumerable VisibleStackModules =>
        AllModules.Where(m => m.Stage == SelectedStage);

    /// <summary>ALL schema modules, ordered spawn → init → update with stage
    /// badges (nothing hidden behind the stage filter — Niagara-style full
    /// palette). Double-click adds an instance to the module's own stage.</summary>
    public IReadOnlyList<PaletteModuleViewModel> PaletteModules
    {
        get
        {
            if (Schema == null) return Array.Empty<PaletteModuleViewModel>();
            int StageRank(string stage) => stage switch
            {
                "spawn" => 0,
                "init" => 1,
                "update" => 2,
                _ => 3,
            };
            return Schema.Modules
                .OrderBy(m => StageRank(m.Stage))
                .ThenBy(m => m.Name, StringComparer.OrdinalIgnoreCase)
                .Select(m => new PaletteModuleViewModel(m))
                .ToList();
        }
    }

    private PfxStackModuleRow? _selectedModule;
    public PfxStackModuleRow? SelectedModule
    {
        get => _selectedModule;
        set
        {
            _selectedModule = value;
            Fire(nameof(SelectedModule));
            Fire(nameof(HasSelectedModule));
            Fire(nameof(NoSelectedModule));
            Fire(nameof(SelectedModuleParams));
            Fire(nameof(HasSelectedCurve));
            Fire(nameof(SelectedCurveParam));
            Fire(nameof(SelectedCurve));
            Fire(nameof(SelectedCurveSlot));
        }
    }

    /// <summary>右侧面板互斥显示用（不用转换器，避免绑定失效隐患）。</summary>
    public bool HasSelectedModule => SelectedModule != null;
    public bool NoSelectedModule => !HasSelectedModule;
    public bool HasSelectedCurve => HasSelectedModule && SelectedCurveParam?.Curve != null;

    // ---- pipeline canvas (Emitter → stack modules → Output) -------------------

    public ObservableCollection<PfxNodeViewModel> Nodes { get; } = new();
    public ObservableCollection<PfxConnectionViewModel> Connections { get; } = new();

    private PfxNodeViewModel? _selectedNode;
    public PfxNodeViewModel? SelectedNode
    {
        get => _selectedNode;
        set { _selectedNode = value; Fire(nameof(SelectedNode)); }
    }

    /// <summary>Canvas node click → drives the parameter panel (terminals
    /// select the root emitter settings).</summary>
    private string _debugState = "(no clicks yet)";
    private static string SharedDebugState = "(no clicks yet)";
    private static readonly System.Collections.Generic.List<PfxEmitterEditorViewModel> LiveViews = new();

    public string DebugState
    {
        get => SharedDebugState;
        private set
        {
            SharedDebugState = value;
            foreach (var v in LiveViews) v.Fire(nameof(DebugState));
        }
    }

    /// <summary>从预览头部的下拉框选择模块行（不经画布节点，事件驱动）。</summary>
    public void SelectStackRow(PfxStackModuleRow row)
    {
        if (row == null) return;
        SelectedNode = null;
        SelectedModule = row;
        DebugState = $"sel={row.Name} (combo) rows={AllModules.Count} has={HasSelectedModule}";
        Logger.Info($"[PFX] module selected from combo: {row.Name}");
    }

    public void SelectNode(PfxNodeViewModel node)
    {
        SelectedNode = node;
        SelectedModule = node.Module;
        DebugState = $"sel={node.Title} | rows={AllModules.Count} | has={HasSelectedModule}";
        Logger.Info($"[PFX] node selected: {(node.Module?.Name ?? node.Title)}");
    }

    /// <summary>Rebuild the pipeline graph from the stack: Emitter → each
    /// module in execution order → Output, chained stage-flow edges.</summary>
    private void RebuildGraph()
    {
        Nodes.Clear();
        Connections.Clear();

        var source = new PfxNodeViewModel
        {
            ModuleName = "Emitter",
            Subtitle = "spawn source",
            IsTerminal = true,
            Title = "Emitter",
        };
        source.Input.Add(new PfxPinViewModel());
        source.Output.Add(new PfxPinViewModel());
        source.Location = new Avalonia.Point(30, 40);
        Nodes.Add(source);

        var previous = source;
        var i = 0;
        foreach (var mod in AllModules)
        {
            var node = new PfxNodeViewModel
            {
                ModuleName = mod.Name,
                Subtitle = mod.Stage,
                Module = mod,
                Title = mod.Name,
            };
            node.Input.Add(new PfxPinViewModel());
            node.Output.Add(new PfxPinViewModel());
            node.Location = new Avalonia.Point(220 + i % 4 * 195, 40 + i / 4 * 125);
            Nodes.Add(node);
            Connections.Add(new PfxConnectionViewModel(previous.Output[0], node.Input[0]));
            previous = node;
            ++i;
        }

        var output = new PfxNodeViewModel
        {
            ModuleName = "Output",
            Subtitle = "render",
            IsTerminal = true,
            Title = "Output",
        };
        output.Input.Add(new PfxPinViewModel());
        output.Output.Add(new PfxPinViewModel());
        output.Location = new Avalonia.Point(220 + i % 4 * 195, 40 + i / 4 * 125);
        Nodes.Add(output);
        Connections.Add(new PfxConnectionViewModel(previous.Output[0], output.Input[0]));
    }

    public ObservableCollection<PfxParamRow> SelectedModuleParams =>
        SelectedModule?.Params ?? new ObservableCollection<PfxParamRow>();

    /// <summary>First curve parameter of the selected module (drives the
    /// embedded over-life curve editor), or null.</summary>
    public PfxParamRow? SelectedCurveParam =>
        SelectedModule?.Params.FirstOrDefault(p => p.IsCurve);

    public ParticleCurveData? SelectedCurve => SelectedCurveParam?.Curve;

    /// <summary>Legacy curve slot for the value-range mapping of the shared
    /// EmitterCurveEditor, inferred from the module name.</summary>
    public ParticleCurveSlot SelectedCurveSlot => CurveSlotForModule(SelectedModule?.Name);

    // ---- root emitter settings (name / blend / capacity) ----------------------

    public string EmitterName
    {
        get => _rootName;
        set { if (_rootName == value) return; _rootName = value; Fire(nameof(EmitterName)); OnStructureEdited("rename emitter"); }
    }
    private string _rootName = "";

    public string BlendMode
    {
        get => _blendMode;
        set { if (_blendMode == value) return; _blendMode = value; Fire(nameof(BlendMode)); OnStructureEdited("blend"); }
    }
    private string _blendMode = "additive";

    public static IReadOnlyList<string> BlendOptions { get; } =
        new[] { "additive", "alpha", "multiply", "premultiplied" };

    // 渲染器类型（§7-N2）：billboard = sprite 兼容桥；mesh = ParticleFXMeshPass 实例
    public string EmitterRenderer
    {
        get => _renderer;
        set
        {
            if (_renderer == value) return;
            _renderer = value;
            Fire(nameof(EmitterRenderer));
            Fire(nameof(IsRenderMesh));
            OnStructureEdited("renderer");
        }
    }
    private string _renderer = "billboard";

    public static IReadOnlyList<string> RendererOptions { get; } = new[] { "billboard", "mesh" };

    public bool IsRenderMesh => string.Equals(_renderer, "mesh", StringComparison.OrdinalIgnoreCase);

    // ---- 渲染 instance 几何源（renderer=mesh 时生效；空 = 内置立方体回退）----------
    // 引擎侧按路径懒加载 cooked .mesh（Bridges load_instance_geometry）；
    // guid 仅供编辑器回读时反查显示，不是引擎事实源。
    public IReadOnlyList<string> InstanceMeshOptions => _instanceMeshOptions;
    private readonly List<string> _instanceMeshOptions = new();
    private readonly List<AssetInfo> _instanceMeshAssets = new();

    public string? InstanceMeshSelected
    {
        get => _instanceMeshSelected;
        set
        {
            if (_instanceMeshSelected == value) return;
            _instanceMeshSelected = value;
            Fire(nameof(InstanceMeshSelected));
            if (value is not null && BuiltInInstanceOptions.TryGetValue(value, out var pseudo))
            {
                // 内置程序化几何：引擎按伪路径解析（builtin:cube/sphere），无需资产
                _instanceMeshPath = pseudo;
                _instanceMeshGuid = "";
            }
            else
            {
                var info = _instanceMeshAssets.FirstOrDefault(a => InstanceMeshDisplayName(a) == value);
                if (info is null)
                {
                    _instanceMeshPath = "";
                    _instanceMeshGuid = "";
                }
                else
                {
                    var cooked = Native.CookedArtifactResolver.ResolveCookedPath(info);
                    _instanceMeshPath = cooked is null
                        ? ""
                        : Native.CookedArtifactResolver.ToCwdRelative(cooked);
                    _instanceMeshGuid = info.Guid.ToString();
                }
            }
            OnStructureEdited("instance.mesh");
            UpdateInstanceMeshRadius();
        }
    }
    private string? _instanceMeshSelected;
    private string _instanceMeshPath = "";
    private string _instanceMeshGuid = "";

    // ---- 实例网格半径（缩放 1 时的包围球半径）→ 碰撞半径对齐用 ---------------
    private string _instanceMeshRadiusText = "";
    public string InstanceMeshRadiusText
    {
        get => _instanceMeshRadiusText;
        private set { if (_instanceMeshRadiusText != value) { _instanceMeshRadiusText = value; Fire(nameof(InstanceMeshRadiusText)); } }
    }

    /// <summary>查询引擎侧网格包围球半径并更新提示文本。</summary>
    private void UpdateInstanceMeshRadius()
    {
        if (string.IsNullOrEmpty(_instanceMeshPath))
        {
            InstanceMeshRadiusText = "";
            return;
        }
        var r = PrimalEditor_Avalonia.Native.NativeEngineBridge.PfxInstanceMeshRadius(_instanceMeshPath);
        InstanceMeshRadiusText = r > 0
            ? $"instance radius ≈ {r:0.###} m — PhysicsCollision radius 填同值（缩放自动跟随）"
            : "";
    }

    /// <summary>一键把 PhysicsCollision.radius 填成实例网格半径（缩放 1 基准）。</summary>
    public void FillCollisionRadiusFromMesh()
    {
        if (string.IsNullOrEmpty(_instanceMeshPath)) { StatusLine = "Fill radius: no instance mesh"; return; }
        var r = PrimalEditor_Avalonia.Native.NativeEngineBridge.PfxInstanceMeshRadius(_instanceMeshPath);
        if (r <= 0) { StatusLine = "Fill radius: mesh radius unknown"; return; }
        foreach (var row in AllModules)
        {
            if (!string.Equals(row.Name, "PhysicsCollision", StringComparison.OrdinalIgnoreCase)) continue;
            var radiusParam = row.Params.FirstOrDefault(p => p.Name == "radius");
            if (radiusParam == null) continue;
            radiusParam.Value = r.ToString("0.###");
            OnParamEdited(radiusParam);
            StatusLine = $"PhysicsCollision radius ← {r:0.###} m";
            return;
        }
        StatusLine = "Fill radius: add a PhysicsCollision module first";
    }

    // ---- P2:逐粒子刚体（instance.physics;引擎侧出生建 Jolt 动态体、物理权威回写）----
    public static IReadOnlyList<string> PhysicsModeOptions { get; } = new[] { "Off", "Rigid Bodies" };

    public string PhysicsMode
    {
        get => _physicsMode;
        set
        {
            if (_physicsMode == value) return;
            _physicsMode = value;
            Fire(nameof(PhysicsMode));
            Fire(nameof(IsPhysicsEnabled));
            OnStructureEdited("instance.physics.mode");
        }
    }
    private string _physicsMode = "Off";

    public bool IsPhysicsEnabled => string.Equals(_physicsMode, "Rigid Bodies", StringComparison.OrdinalIgnoreCase);

    public string PhysicsRadius
    {
        get => _physicsRadius;
        set { if (_physicsRadius == value) return; _physicsRadius = value; Fire(nameof(PhysicsRadius)); OnStructureEdited("instance.physics.radius"); }
    }
    private string _physicsRadius = "0.05";

    public string PhysicsMass
    {
        get => _physicsMass;
        set { if (_physicsMass == value) return; _physicsMass = value; Fire(nameof(PhysicsMass)); OnStructureEdited("instance.physics.mass"); }
    }
    private string _physicsMass = "1";

    public string PhysicsMaxBodies
    {
        get => _physicsMaxBodies;
        set { if (_physicsMaxBodies == value) return; _physicsMaxBodies = value; Fire(nameof(PhysicsMaxBodies)); OnStructureEdited("instance.physics.max_bodies"); }
    }
    private string _physicsMaxBodies = "256";

    /// <summary>自碰撞:kDynamic(互相排开堆叠,默认)/ 关 → kParticle(只碰世界,互相穿透)。
    /// 引擎经对象层矩阵控制(Jolt 宽阶段零成本跳过)。</summary>
    public bool PhysicsSelfCollision
    {
        get => _physicsSelfCollision;
        set { if (_physicsSelfCollision == value) return; _physicsSelfCollision = value; Fire(nameof(PhysicsSelfCollision)); OnStructureEdited("instance.physics.self_collision"); }
    }
    private bool _physicsSelfCollision = true;

    private static string InstanceMeshDisplayName(AssetInfo a) => a.FileName;

    // 内置程序化 instance 几何（引擎 ParticleFXMeshPass 槽 0/1，无需内容资产）
    public const string BuiltInCubeOption = "Built-in Cube (内置立方体)";
    public const string BuiltInSphereOption = "Built-in Sphere (内置球体)";
    private static readonly System.Collections.Generic.Dictionary<string, string>
        BuiltInInstanceOptions = new()
    {
        [BuiltInCubeOption] = "builtin:cube",
        [BuiltInSphereOption] = "builtin:sphere",
    };

    /// <summary>按 AssetRegistry 当前内容重建 mesh 选项（Mesh/Model 资产，名称排序）。
    /// 内置 Cube/Sphere 恒在最前。编辑器打开资产（SetAsset→LoadFromJson）时调用。</summary>
    private void RefreshInstanceMeshOptions()
    {
        _instanceMeshAssets.Clear();
        _instanceMeshOptions.Clear();
        _instanceMeshAssets.AddRange(AssetRegistry.Assets
            .Where(a => a.Type == Native.AssetType.Mesh || a.Type == Native.AssetType.Model)
            .OrderBy(a => a.FileName, StringComparer.OrdinalIgnoreCase));
        _instanceMeshOptions.Add(BuiltInCubeOption);
        _instanceMeshOptions.Add(BuiltInSphereOption);
        _instanceMeshOptions.AddRange(_instanceMeshAssets.Select(InstanceMeshDisplayName));
        Fire(nameof(InstanceMeshOptions));
    }

    /// <summary>存储的 instance 路径/guid → ComboBox 显示名（guid 反查优先，退回路径）。</summary>
    private string? InstanceMeshDisplayFor(string path, string guid)
    {
        foreach (var (label, pseudo) in BuiltInInstanceOptions)
            if (string.Equals(path, pseudo, StringComparison.OrdinalIgnoreCase)) return label;
        if (Guid.TryParse(guid, out var g)
            && AssetRegistry.GetByGuid(g) is { } info)
            return InstanceMeshDisplayName(info);
        return string.IsNullOrEmpty(path) ? null : path;
    }

    public string Capacity
    {
        get => _capacity;
        set { if (_capacity == value) return; _capacity = value; Fire(nameof(Capacity)); OnStructureEdited("capacity"); }
    }
    private string _capacity = "4096";

    // ---- dirty / status --------------------------------------------------------

    private bool _isDirty;
    public new bool IsDirty
    {
        get => _isDirty;
        private set { if (_isDirty != value) { _isDirty = value; Fire(nameof(IsDirty)); Fire(nameof(DirtyMarker)); } }
    }
    public override void MarkDirty() => IsDirty = true;
    public override void ClearDirty() => IsDirty = false;
    public string DirtyMarker => _isDirty ? "● unsaved" : string.Empty;

    public string StatusLine
    {
        get => _statusLine;
        private set { if (_statusLine != value) { _statusLine = value; Fire(nameof(StatusLine)); } }
    }
    private string _statusLine = "Ready";

    public override string Title => Asset != null ? $"PFX Emitter — {Asset.FileName}" : "PFX Emitter";

    public override (int W, int H)? PreferredInitialSize => (1500, 950);

    public PfxEmitterEditorViewModel()
    {
        Schema = PfxEditorService.Instance.Schema;
        LiveViews.Add(this);
    }

    // 编辑器单实例语义：同一进程内所有 PFX 编辑器窗口共享同一个 VM 状态
    // （与 AssetEditorRegistry 的按路径去重配合；跨实例的选中状态分裂
    //  曾导致"点击节点无反应"——静态化后不可能再分裂）。
    private static PfxEmitterEditorViewModel? _current;
    public static PfxEmitterEditorViewModel Current => _current ??= new PfxEmitterEditorViewModel();

    // ============================================================
    //  Asset lifecycle
    // ============================================================

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        if (Schema == null)
        {
            PfxEditorService.Instance.LoadSchema();
            Schema = PfxEditorService.Instance.Schema;
            Fire(nameof(PaletteModules));
        }
        try
        {
            _json = File.ReadAllText(info.FullPath);
        }
        catch (Exception ex)
        {
            Logger.Error($"PfxEmitterEditorViewModel.SetAsset read failed: {ex.Message}", ex);
            _json = "{}";
        }
        LoadFromJson(_json);
        _lastSnapshot = _json; // undo baseline: the freshly-loaded document
        RefreshSceneEntities(); // 目标实体下拉随资产打开刷新
        UpdateInstanceMeshRadius(); // 实例半径提示随加载刷新
        ClearDirty();
        StatusLine = "Ready";
    }

    public override void Save()
    {
        if (Asset == null) return;
        try
        {
            SyncToJson();
            File.WriteAllText(Asset.FullPath, _json);
            ClearDirty();
            StatusLine = $"Saved {Asset.FileName}";
            PushHotPreview();
            Logger.Info($"Saved PFX emitter: {Asset.FileName}");
            // 场景热更新：重建所有使用该资产的活动发射器（回调由
            // WorldEditorViewModel 注入 → SceneRuntimeSynchronizer）。
            AssetSavedOnDiskCallback?.Invoke(Asset.FullPath);
        }
        catch (Exception ex)
        {
            StatusLine = $"Save failed: {ex.Message}";
            Logger.Error($"PFX emitter save failed: {ex.Message}", ex);
        }
    }

    public override void Discard()
    {
        if (Asset == null) return;
        SetAsset(Asset);
    }

    // ============================================================
    //  Scene binding（把资产挂到场景实体——回答"哪个实例用它发射"）
    // ============================================================

    /// <summary>静态回调由 WorldEditorViewModel 注入：把资产挂到 Hierarchy 当前
    /// 选中实体（Bind 未选下拉目标时的回退路径）。null = 失败（原因已写日志）。</summary>
    public static Func<string, string?>? BindToSelectedEntityCallback { get; set; }

    /// <summary>静态回调由 WorldEditorViewModel 注入：Save 落盘后重建场景中
    /// 使用该资产的活动发射器（热更新，场景无需重载）。</summary>
    public static Action<string>? AssetSavedOnDiskCallback { get; set; }

    /// <summary>场景发射源选择：当前激活场景的实体名列表（null = 无场景）。</summary>
    public static Func<IReadOnlyList<string>>? SceneEntityNamesProvider { get; set; }

    /// <summary>把资产绑定到指定名字的场景实体（新建/复用其 ParticleFX 组件）。
    /// 返回 null = 失败。entityName 用实体名匹配（v1：取第一个同名实体）。</summary>
    public static Func<string, string, string?>? BindToSceneEntityCallback { get; set; }

    public System.Collections.ObjectModel.ObservableCollection<string> SceneEntityOptions { get; } = new();

    private string? _selectedSceneEntity;
    public string? SelectedSceneEntity
    {
        get => _selectedSceneEntity;
        set { if (_selectedSceneEntity != value) { _selectedSceneEntity = value; Fire(nameof(SelectedSceneEntity)); } }
    }

    /// <summary>刷新目标实体下拉（窗口打开/Save 后调用）。</summary>
    public void RefreshSceneEntities()
    {
        var names = SceneEntityNamesProvider?.Invoke();
        SceneEntityOptions.Clear();
        if (names != null) foreach (var n in names) SceneEntityOptions.Add(n);
        Fire(nameof(SceneEntityOptions));
        if (_selectedSceneEntity == null && SceneEntityOptions.Count > 0)
            SelectedSceneEntity = SceneEntityOptions[0];
    }

    /// <summary>工具栏 "Bind"：优先绑定下拉选中的场景实体；未选则回退
    /// 到 Hierarchy 当前选中实体。</summary>
    public void BindToSelectedEntity()
    {
        if (Asset == null)
        {
            StatusLine = "Bind: no asset open";
            return;
        }
        if (!string.IsNullOrEmpty(SelectedSceneEntity))
        {
            var byName = BindToSceneEntityCallback?.Invoke(SelectedSceneEntity, Asset.FullPath);
            StatusLine = byName ?? $"Bind failed — scene entity '{SelectedSceneEntity}' not found";
            return;
        }
        var result = BindToSelectedEntityCallback?.Invoke(Asset.FullPath);
        StatusLine = result ?? "Bind failed — select a target entity (or select one in Hierarchy)";
    }

    // ============================================================
    //  JSON ↔ module rows
    // ============================================================

    private void LoadFromJson(string json)
    {
        _json = string.IsNullOrWhiteSpace(json) ? "{}" : json;
        AllModules.Clear();
        _rootName = "";
        _blendMode = "additive";
        _capacity = "4096";
        try
        {
            var root = System.Text.Json.Nodes.JsonNode.Parse(_json) as System.Text.Json.Nodes.JsonObject;
            if (root?["name"] is System.Text.Json.Nodes.JsonValue nv)
                _rootName = nv.GetValue<string>();
            if (root?["blend"] is System.Text.Json.Nodes.JsonValue bv)
                _blendMode = bv.GetValue<string>();
            if (root?["capacity"] is System.Text.Json.Nodes.JsonValue cv)
                _capacity = cv.ToJsonString().Trim('"');
            if (root?["renderer"] is System.Text.Json.Nodes.JsonValue rv)
                _renderer = rv.GetValue<string>();
            if (root?["instance"] is System.Text.Json.Nodes.JsonObject inst)
            {
                _instanceMeshPath = inst["mesh"] is System.Text.Json.Nodes.JsonValue mv
                    ? mv.GetValue<string>() : "";
                _instanceMeshGuid = inst["guid"] is System.Text.Json.Nodes.JsonValue gv
                    ? gv.GetValue<string>() : "";
                if (inst["physics"] is System.Text.Json.Nodes.JsonObject ph)
                {
                    var mode = ph["mode"]?.GetValue<string>();
                    _physicsMode = mode == "rigid_body" ? "Rigid Bodies" : "Off";
                    _physicsRadius = ph["radius"]?.ToJsonString().Trim('"') is { } r && float.TryParse(r, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out _) ? r : "0.05";
                    _physicsMass = ph["mass"]?.ToJsonString().Trim('"') is { } m && float.TryParse(m, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out _) ? m : "1";
                    _physicsMaxBodies = ph["max_bodies"]?.ToJsonString().Trim('"') is { } b && int.TryParse(b, out _) ? b : "256";
                    _physicsSelfCollision = ph["self_collision"] is not System.Text.Json.Nodes.JsonValue sc || sc.GetValue<bool>();
                }
                else
                {
                    _physicsMode = "Off";
                    _physicsSelfCollision = true;
                }
            }
            else
            {
                _instanceMeshPath = "";
                _instanceMeshGuid = "";
                _physicsMode = "Off";
                _physicsSelfCollision = true;
            }
            RefreshInstanceMeshOptions();
            _instanceMeshSelected = InstanceMeshDisplayFor(_instanceMeshPath, _instanceMeshGuid);
            Fire(nameof(InstanceMeshSelected));
            Fire(nameof(IsRenderMesh));
            if (root?["stages"] is System.Text.Json.Nodes.JsonObject stages)
            {
                foreach (var stage in _stageNames)
                {
                    if (stages[stage] is not System.Text.Json.Nodes.JsonArray arr) continue;
                    foreach (var mod in arr.OfType<System.Text.Json.Nodes.JsonObject>())
                    {
                        var name = mod["module"]?.GetValue<string>() ?? "?";
                        var row = new PfxStackModuleRow(stage, name);
                        foreach (var kv in mod)
                        {
                            if (kv.Key == "module") continue;
                            row.Params.Add(BuildParamRow(name, kv.Key, kv.Value));
                        }
                        // 文档缺失、但引擎 schema 给了默认值的参数（如 AddVelocity
                        // 的 direction）：按引擎默认值补出可编辑行，否则表单不
                        // 显示、用户无从设置。
                        MaterializeMissingDefaults(row);
                        SortParamsBySchema(row);
                        AllModules.Add(row);
                    }
                }
            }
        }
        catch (Exception ex)
        {
            StatusLine = $"Parse failed: {ex.Message}";
            Logger.Error($"PFX editor parse failed: {ex.Message}", ex);
        }
        Fire(nameof(EmitterName));
        Fire(nameof(BlendMode));
        Fire(nameof(Capacity));
        Fire(nameof(EmitterRenderer));
        Fire(nameof(AllModules));
        Fire(nameof(VisibleStackModules));
        if (SelectedModule != null && !AllModules.Contains(SelectedModule))
            SelectedModule = AllModules.FirstOrDefault();
        else
            Fire(nameof(SelectedModule));
        SelectedStage = _stageNames.Contains(SelectedStage) ? SelectedStage : "spawn";
        RebuildGraph();
    }

    /// <summary>Schema-typed param row for a raw JSON member of a module object.</summary>
    private PfxParamRow BuildParamRow(string moduleName, string key, System.Text.Json.Nodes.JsonNode? value)
    {
        var hint = Schema?.Modules
            .FirstOrDefault(m => string.Equals(m.Name, moduleName, StringComparison.OrdinalIgnoreCase))?
            .Params.FirstOrDefault(p => string.Equals(p.Name, key, StringComparison.OrdinalIgnoreCase));
        var type = hint?.Type ?? InferType(value);
        var note = hint?.Note ?? "";
        return BuildParamRowTyped(moduleName, key, type, note, value);
    }

    private static string InferType(System.Text.Json.Nodes.JsonNode? value) => value switch
    {
        System.Text.Json.Nodes.JsonArray => "curve",
        System.Text.Json.Nodes.JsonValue v when v.TryGetValue<bool>(out _) => "bool",
        System.Text.Json.Nodes.JsonValue v when v.TryGetValue<string>(out _) => "param_ref",
        _ => "f32",
    };

    private PfxParamRow BuildParamRowTyped(string moduleName, string key, string type, string note,
        System.Text.Json.Nodes.JsonNode? value)
    {
        var kind = type switch
        {
            "f32" or "u32" => PfxParamRow.ParamKind.Numeric,
            "v2" or "v3" or "v4" => PfxParamRow.ParamKind.Vector,
            "bool" => PfxParamRow.ParamKind.Bool,
            "curve" => PfxParamRow.ParamKind.Curve,
            "enum" => PfxParamRow.ParamKind.Enum,
            // P4.2:asset 类型 → 资产选择器(选项 = 项目内 OBJ 源资产的 CWD 相对路径,
            // 与引擎 ShapeLocation mesh 的存储格式一致;选中即写值)
            "asset" => PfxParamRow.ParamKind.Asset,
            // param_ref 的数字形态必须按 Numeric 编辑/序列化：引擎侧
            // get_param_ref_field 只认 number 或 {"param":...} 绑定对象，
            // 写成 JSON 字符串会编译失败（reload 被拒 → 预览保持旧程序）。
            "param_ref" when value is not System.Text.Json.Nodes.JsonObject
                => PfxParamRow.ParamKind.Numeric,
            _ => PfxParamRow.ParamKind.Text,
        };
        var row = new PfxParamRow(key, type, note, kind, () => { })
        {
            Initializing = true,
            ModuleName = moduleName,
            CurveSlot = CurveSlotForModule(moduleName),
        };
        row.SetChangedCallback(() => OnParamEdited(row));
        if (kind == PfxParamRow.ParamKind.Asset)
        {
            // P4.2:OBJ 源资产(引擎 ShapeLocation mesh 解析器只吃 OBJ)→ CWD 相对路径选项
            row.AssetOptions = AssetRegistry.Assets
                .Where(a => a.Type == Native.AssetType.Mesh &&
                            string.Equals(a.Extension, ".obj", StringComparison.OrdinalIgnoreCase))
                .OrderBy(a => a.FileName, StringComparer.OrdinalIgnoreCase)
                .Select(a => Native.CookedArtifactResolver.ToCwdRelative(a.FullPath))
                .ToArray();
        }
        switch (kind)
        {
            case PfxParamRow.ParamKind.Numeric:
                row.Value = value?.ToJsonString().Trim('"') ?? "0";
                break;
            case PfxParamRow.ParamKind.Vector:
            {
                row.ComponentCount = type == "v2" ? 2 : type == "v4" ? 4 : 3;
                var comps = ParseVector(value, row.ComponentCount);
                row.X = comps[0]; row.Y = comps[1]; row.Z = comps[2]; row.W = comps[3];
                break;
            }
            case PfxParamRow.ParamKind.Bool:
                row.BoolValue = value?.GetValueKind() == System.Text.Json.JsonValueKind.True;
                break;
            case PfxParamRow.ParamKind.Enum:
                row.Value = value?.ToJsonString().Trim('"') ?? "";
                row.EnumOptions = EnumOptionsFromNote(note);
                break;
            case PfxParamRow.ParamKind.Curve:
                row.Curve = ParseCurve(value, CurveSlotForModule(moduleName));
                break;
            default:
                row.Value = value?.ToJsonString().Trim('"') ?? "";
                break;
        }
        row.Initializing = false;
        return row;
    }

    /// <summary>Order a loaded module's params the way the schema lists them.</summary>
    private void SortParamsBySchema(PfxStackModuleRow row)
    {
        var order = Schema?.Modules
            .FirstOrDefault(m => string.Equals(m.Name, row.Name, StringComparison.OrdinalIgnoreCase))?.Params;
        if (order == null || order.Count == 0) return;
        var ranked = row.Params
            .OrderBy(p =>
            {
                int i = order.FindIndex(h => string.Equals(h.Name, p.Name, StringComparison.OrdinalIgnoreCase));
                return i >= 0 ? i : int.MaxValue;
            })
            .ToList();
        row.Params.Clear();
        foreach (var p in ranked) row.Params.Add(p);
    }

    /// <summary>为文档缺失、但引擎 schema 声明了 default 的参数补出参数行。
    /// 只补 default 非空的参数——"存在即启用"型参数（scale_end/color_end）
    /// 引擎侧不声明 default，缺席时保持缺席，语义不被编辑器改变。</summary>
    private void MaterializeMissingDefaults(PfxStackModuleRow row)
    {
        var hints = Schema?.Modules
            .FirstOrDefault(m => string.Equals(m.Name, row.Name, StringComparison.OrdinalIgnoreCase))?.Params;
        if (hints == null || hints.Count == 0) return;
        foreach (var hint in hints)
        {
            if (hint.Default is not { } def) continue;
            if (row.Params.Any(p => string.Equals(p.Name, hint.Name, StringComparison.OrdinalIgnoreCase))) continue;
            try
            {
                row.Params.Add(BuildParamRowTyped(row.Name, hint.Name, hint.Type, hint.Note,
                    System.Text.Json.Nodes.JsonNode.Parse(def.GetRawText())));
            }
            catch
            {
                // 引擎 default 串异常：跳过该参数，不阻塞整个模块加载
            }
        }
    }

    /// <summary>enum 参数的可选值从 schema note 的管道分隔格式解析（如 "fixed|shape_normal"）。</summary>
    private static string[] EnumOptionsFromNote(string note) =>
        note.Split('|', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

    private static string[] ParseVector(System.Text.Json.Nodes.JsonNode? value, int count)
    {
        var comps = new string[Math.Max(4, count)];
        for (int i = 0; i < comps.Length; ++i) comps[i] = "0";
        if (value is System.Text.Json.Nodes.JsonArray arr)
        {
            for (int i = 0; i < count && i < arr.Count; ++i)
                comps[i] = arr[i]?.ToJsonString().Trim('"') ?? "0";
        }
        return comps;
    }

    /// <summary>[[t,v]...] / [[t,x,y,z]...] / [[t,r,g,b,a]...] → curve model.</summary>
    private static ParticleCurveData? ParseCurve(System.Text.Json.Nodes.JsonNode? value, ParticleCurveSlot slot)
    {
        if (value is not System.Text.Json.Nodes.JsonArray outer || outer.Count == 0) return null;
        int comps = 1;
        if (outer[0] is System.Text.Json.Nodes.JsonArray first && first.Count > 1)
            comps = Math.Clamp(first.Count - 1, 1, 4);
        var data = new ParticleCurveData { Slot = slot };
        data.Keys.Clear();
        foreach (var node in outer.OfType<System.Text.Json.Nodes.JsonArray>())
        {
            if (node.Count < 2) continue;
            var key = new ParticleCurveKey { Time = ToFloat(node[0]) };
            key.Value = new float[comps];
            for (int i = 0; i < comps; ++i)
                key.Value[i] = i + 1 < node.Count ? ToFloat(node[i + 1]) : 0f;
            data.Keys.Add(key);
        }
        data.Keys.Sort((a, b) => a.Time.CompareTo(b.Time));
        return data;
    }

    private static float ToFloat(System.Text.Json.Nodes.JsonNode? node) =>
        float.TryParse(node?.ToJsonString().Trim('"'), NumberStyles.Float, CultureInfo.InvariantCulture, out var f) ? f : 0f;

    /// <summary>Over-life module names map onto the shared editor's legacy
    /// curve slots so the value range shown matches the quantity edited.</summary>
    private static ParticleCurveSlot CurveSlotForModule(string? moduleName)
    {
        var n = moduleName ?? "";
        if (n.Contains("Alpha", StringComparison.OrdinalIgnoreCase)) return ParticleCurveSlot.Alpha;
        if (n.Contains("Rotation", StringComparison.OrdinalIgnoreCase)) return ParticleCurveSlot.Rotation;
        if (n.Contains("Force", StringComparison.OrdinalIgnoreCase)) return ParticleCurveSlot.Force;
        if (n.Contains("Color", StringComparison.OrdinalIgnoreCase)) return ParticleCurveSlot.ColorGradient;
        if (n.Contains("Velocity", StringComparison.OrdinalIgnoreCase)) return ParticleCurveSlot.Velocity;
        return ParticleCurveSlot.Scale;
    }

    /// <summary>Module rows → JSON document (preserves unknown root keys).</summary>
    private void SyncToJson()
    {
        var root = System.Text.Json.Nodes.JsonNode.Parse(_json) as System.Text.Json.Nodes.JsonObject ?? new System.Text.Json.Nodes.JsonObject();
        root["name"] = _rootName;
        root["blend"] = _blendMode;
        root["renderer"] = _renderer;
        if (!string.IsNullOrEmpty(_instanceMeshPath) || IsPhysicsEnabled)
        {
            var inst = root["instance"] as System.Text.Json.Nodes.JsonObject
                       ?? new System.Text.Json.Nodes.JsonObject();
            if (!string.IsNullOrEmpty(_instanceMeshPath))
            {
                inst["mesh"] = _instanceMeshPath;
                // guid:空值时显式移除——节点复用会让旧文档的残留键"复活"
                if (!string.IsNullOrEmpty(_instanceMeshGuid)) inst["guid"] = _instanceMeshGuid;
                else inst.Remove("guid");
            }
            if (IsPhysicsEnabled)
            {
                var ph = inst["physics"] as System.Text.Json.Nodes.JsonObject
                         ?? new System.Text.Json.Nodes.JsonObject();
                ph["mode"] = "rigid_body";
                if (float.TryParse(_physicsRadius, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var rad)) ph["radius"] = rad;
                if (float.TryParse(_physicsMass, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var mass)) ph["mass"] = mass;
                if (int.TryParse(_physicsMaxBodies, out var mb) && mb > 0) ph["max_bodies"] = mb;
                // 自碰撞开 = 键省略(缺省 true);必须显式移除旧键,否则文档残留的 false 会存活
                if (_physicsSelfCollision) ph.Remove("self_collision");
                else ph["self_collision"] = false;
                inst["physics"] = ph;
            }
            else if (inst["physics"] is not null)
            {
                inst.Remove("physics");
            }
            root["instance"] = inst;
        }
        else if (root["instance"] is not null)
        {
            root.Remove("instance");
        }
        if (int.TryParse(_capacity, out var cap) && cap > 0) root["capacity"] = cap;

        var stages = root["stages"] as System.Text.Json.Nodes.JsonObject ?? new System.Text.Json.Nodes.JsonObject();
        root["stages"] = stages;
        foreach (var stage in _stageNames)
        {
            var arr = new System.Text.Json.Nodes.JsonArray();
            foreach (var mod in AllModules.Where(m => m.Stage == stage))
            {
                var obj = new System.Text.Json.Nodes.JsonObject { ["module"] = mod.Name };
                foreach (var p in mod.Params)
                    obj[p.Name] = ParamToJson(p);
                arr.Add(obj);
            }
            stages[stage] = arr;
        }
        _json = root.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
    }

    private static System.Text.Json.Nodes.JsonNode? ParamToJson(PfxParamRow p) => p.Kind switch
    {
        PfxParamRow.ParamKind.Vector => new System.Text.Json.Nodes.JsonArray(
            Enumerable.Range(0, p.ComponentCount)
                .Select(i => ToNum(i switch { 0 => p.X, 1 => p.Y, 2 => p.Z, _ => p.W }))
                .ToArray()),
        PfxParamRow.ParamKind.Bool => System.Text.Json.Nodes.JsonValue.Create(p.BoolValue),
        PfxParamRow.ParamKind.Curve => CurveToJson(p),
        PfxParamRow.ParamKind.Numeric => ToNum(p.Value),
        _ => TextToJson(p.Value),
    };

    /// <summary>自由文本 → JSON：纯数字写成 number（param_ref 兜底，引擎只认
    /// number/绑定对象）、{...} 文本解析回对象（param 绑定原样往返）、其余按
    /// 字符串（enum 值等）。</summary>
    private static System.Text.Json.Nodes.JsonNode? TextToJson(string? text)
    {
        var s = text?.Trim() ?? "";
        if (s.Length == 0) return System.Text.Json.Nodes.JsonValue.Create("");
        if (float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var f))
            return System.Text.Json.Nodes.JsonValue.Create(f);
        if (s[0] == '{')
        {
            try
            {
                if (System.Text.Json.Nodes.JsonNode.Parse(s) is { } parsed) return parsed;
            }
            catch { /* 非法对象文本 → 按字符串保留，不丢用户输入 */ }
        }
        return System.Text.Json.Nodes.JsonValue.Create(s);
    }

    private static System.Text.Json.Nodes.JsonNode? ToNum(string text) =>
        float.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var f)
            ? System.Text.Json.Nodes.JsonValue.Create(f)
            : System.Text.Json.Nodes.JsonValue.Create(text);

    private static System.Text.Json.Nodes.JsonNode? CurveToJson(PfxParamRow p)
    {
        var curve = p.Curve;
        if (curve == null || curve.Keys.Count == 0) return null;
        var comps = curve.ValueCount;
        var arr = new System.Text.Json.Nodes.JsonArray();
        foreach (var key in curve.Keys)
        {
            var entry = new System.Text.Json.Nodes.JsonArray { key.Time };
            for (int i = 0; i < comps; ++i)
                entry.Add(i < key.Value.Length ? key.Value[i] : 0f);
            arr.Add(entry);
        }
        return arr;
    }

    // ============================================================
    //  Editing operations
    // ============================================================

    /// <summary>Schema palette double-click: append a module instance to the
    /// selected stage with default (empty/zero) parameters.</summary>
    public void AddModuleFromPalette(PaletteModuleViewModel paletteItem)
    {
        if (paletteItem?.Schema == null) return;
        var def = paletteItem.Schema;
        var row = new PfxStackModuleRow(def.Stage, def.Name);
        foreach (var hint in def.Params)
            row.Params.Add(BuildParamRowTyped(def.Name, hint.Name, hint.Type, hint.Note, null));
        AllModules.Add(row);
        SelectedModule = row;
        SelectedStage = def.Stage;
        RebuildGraph();
        OnStructureEdited($"add {def.Name}");
        PushHotPreview();
    }

    /// <summary>按行删除模块实例（每模块头部的 × 按钮）。</summary>
    public void RemoveModuleRow(PfxStackModuleRow row)
    {
        if (row == null || !AllModules.Contains(row)) return;
        var name = row.Name;
        AllModules.Remove(row);
        RebuildGraph();
        OnStructureEdited($"remove {name}");
        PushHotPreview();
    }

    private RelayCommand? _removeModuleCommand;
    public System.Windows.Input.ICommand RemoveModuleCommand =>
        _removeModuleCommand ??= new RelayCommand(Parameter =>
        {
            if (Parameter is PfxStackModuleRow row) RemoveModuleRow(row);
        });
    private void OnParamEdited(PfxParamRow row)
    {
        MarkDirty();
        SnapshotPending();
        PushHotPreview();
    }

    /// <summary>Structural edits (add/remove/rename/root fields): immediate
    /// undo snapshot + JSON resync.</summary>
    private void OnStructureEdited(string label)
    {
        SyncToJson();
        PushUndoSnapshot(label);
        MarkDirty();
        Fire(nameof(VisibleStackModules));
    }

    // ---- undo / redo (JSON snapshots via the host's Ctrl+Z) --------------------

    private string _lastSnapshot = "{}";

    private void PushUndoSnapshot(string label)
    {
        SyncToJson();
        var before = _lastSnapshot;
        var after = _json;
        if (before == after) return;
        _lastSnapshot = after;
        RecordChange(new JsonSnapshotAction(this, before, after, label));
    }

    /// <summary>Coalesce rapid text edits into one snapshot after 700ms idle.</summary>
    private void SnapshotPending()
    {
        _snapshotTimer ??= new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(700) };
        _snapshotTimer.Stop();
        _snapshotTimer.Start();
    }

    private DispatcherTimer? _snapshotTimer;
    private bool _snapshotTimerWired;

    /// <summary>Wire the coalescing timer (called once from the view's ctor).</summary>
    public void AttachUndoCoalescer()
    {
        if (_snapshotTimerWired) return;
        _snapshotTimerWired = true;
        _snapshotTimer ??= new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(700) };
        _snapshotTimer.Tick += (_, _) =>
        {
            _snapshotTimer!.Stop();
            PushUndoSnapshot("edit");
        };
    }

    private void ApplyJsonSnapshot(string json)
    {
        LoadFromJson(json);
        _lastSnapshot = json;
        MarkDirty();
    }

    private sealed class JsonSnapshotAction : IUndoRedoAction
    {
        private readonly PfxEmitterEditorViewModel _vm;
        private readonly string _before;
        private readonly string _after;
        public string Name { get; }

        public JsonSnapshotAction(PfxEmitterEditorViewModel vm, string before, string after, string label)
        {
            _vm = vm;
            _before = before;
            _after = after;
            Name = $"PFX {label}";
        }

        public void Undo() => _vm.ApplyJsonSnapshot(_before);
        public void Redo() => _vm.ApplyJsonSnapshot(_after);
    }

    // ---- hot preview (resident) -------------------------------------------------

    private DispatcherTimer? _previewTimer;
    private uint _lastPushedEmitterId;

    /// <summary>常驻预览：编辑器打开期间周期性把当前程序推送到使用该资产的
    /// 活场景发射器（引擎侧持续播放，无需手动按键）。Start once from the view.</summary>
    public void StartLivePreviewLoop()
    {
        if (_previewTimer != null) return;
        _previewTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(800) };
        _previewTimer.Tick += (_, _) => PushHotPreview();
        _previewTimer.Start();
    }

    /// <summary>Push the edited program into a live scene emitter using this
    /// asset (PfxEmitterReloadFromMemory). No live user → status hint.</summary>
    private bool _pushRejected;

    public void PushHotPreview()
    {
        if (Asset == null || !NativeEngineBridge.IsLoaded) return;
        SyncToJson();
        var bytes = System.Text.Encoding.UTF8.GetBytes(_json);
        var ok = true;

        // 只推私有预览发射器（编辑器内嵌 viewport 的数据源）。
        // 场景里的活发射器不再被窗口内存态直接覆盖——旧实现会在用户仅打开
        // 窗口/改参数时把过期的内存程序推入场景（表现为"改了没作用"或
        // "行为突然变成另一套"）；场景统一走 Save → 落盘 → Resync 链路。
        var previewId = ResolvePreviewEmitterId();
        if (previewId != 0)
        {
            ok &= NativeEngineBridge.PfxEmitterReloadFromMemory(previewId, bytes) != 0;
        }

        // 引擎拒绝编译时保留旧程序继续模拟 —— 表现恰好是"改参数预览没反应"，
        // 必须把拒绝显式亮到状态栏，否则无从排查。
        if (!ok && !_pushRejected)
        {
            StatusLine = "Preview push rejected — program compile failed (check param values)";
            _pushRejected = true;
        }
        else if (ok && _pushRejected)
        {
            StatusLine = "Ready";
            _pushRejected = false;
        }
    }


    // ============================================================
    //  内嵌 3D 预览（Niagara 式 viewport，点显示风格）
    //  数据：PfxGetParticleSnapshot 快照 ABI（活粒子 位置/尺寸/颜色）。
    //  发射器来源：优先场景中挂载该资产的活发射器（所见即所得），
    //  否则自建私有发射器（编辑器关闭时销毁，OnEditorClosed）。
    //  投影与绘制由 ParticlePreviewControl 完成（DrawingContext 自绘）。
    // ============================================================

    private uint _previewEmitterId;
    private bool _ownsPreviewEmitter;
    private readonly float[] _snapshotPos = new float[4096 * 4];
    private readonly float[] _snapshotColor = new float[4096 * 4];

    private uint ResolvePreviewEmitterId()
    {
        if (Asset == null) return 0;
        // 注意：不再把场景活发射器当作预览目标——那会让任何一次预览推送
        // （改参数/撤销/加删模块）把窗口内存 JSON ReloadFromMemory 进场景，
        // 窗口状态与磁盘稍有出入时场景行为就突变。预览永远用私有发射器；
        // 场景发射器只经 Save → AssetSavedOnDiskCallback → Resync 更新。
        if (_previewEmitterId == 0 && NativeEngineBridge.IsLoaded)
        {
            _previewEmitterId = NativeEngineBridge.PfxEmitterCreateFromAsset(Asset.FullPath);
            if (_previewEmitterId != 0)
            {
                _ownsPreviewEmitter = true;
                // 编辑器私有预览发射器不进世界场景渲染（否则主视口会在
                // 世界原点"凭空"画出预览粒子）。
                NativeEngineBridge.PfxSetEditorPreview(_previewEmitterId, preview: true);
            }
        }
        return _previewEmitterId;
    }

    private void ReleaseOwnedPreviewEmitter()
    {
        if (_previewEmitterId == 0) return;
        if (NativeEngineBridge.IsLoaded) NativeEngineBridge.PfxEmitterRemove(_previewEmitterId);
        _previewEmitterId = 0;
        _ownsPreviewEmitter = false;
    }

    /// <summary>曲线编辑完成（由视图的 EmitterCurveEditor 回调）。</summary>
    public void NotifyCurveEdited()
    {
        MarkDirty();
        PushUndoSnapshot("curve edit");
        PushHotPreview();
    }

    /// <summary>控件每帧调用：读取当前活粒子快照。返回 false = 无可显示内容。</summary>
    public bool TryGetPreviewSnapshot(out float[] posSize, out float[] color, out uint count)
    {
        posSize = _snapshotPos;
        color = _snapshotColor;
        count = 0;
        if (Asset == null || !NativeEngineBridge.IsLoaded) return false;
        var id = ResolvePreviewEmitterId();
        if (id == 0) return false;
        count = NativeEngineBridge.PfxGetParticleSnapshot(id, _snapshotPos, _snapshotColor,
            (uint)(_snapshotPos.Length / 4));
        return true;
    }

    /// <summary>编辑器窗口关闭：销毁私有预览发射器（OnEditorClosed）。</summary>
    public override void OnEditorClosed()
    {
        ReleaseOwnedPreviewEmitter();
        LiveViews.Remove(this);
    }

    public void ShutdownPreview() => ReleaseOwnedPreviewEmitter();

    // ---- INPC ---------------------------------------------------------------------
    // 必须走基类 ObservableObject 的 OnPropertyChanged：INPC 接口映射在基类的
    // PropertyChanged 事件上。此前本类重新声明了 new event PropertyChanged，
    // 形成双事件槽——直接 C# 订阅与 XAML/INPC 绑定各听一个，UI 绑定（如
    // IsEnabled="{Binding IsDirty}"、宿主窗口标题）永远收不到通知，
    // Save/Discard 因此永远禁用（改了参数也无法保存，重开即"重置"）。

    private void Fire(string name) => OnPropertyChanged(name);
}

/// <summary>Palette list item wrapping a schema module definition for the
/// stage's module palette (double-click adds an instance to the stack).</summary>
public sealed class PaletteModuleViewModel
{
    public PfxModuleSchema Schema { get; }
    public string DisplayName => Schema.Name;
    public string Note
    {
        get
        {
            var paramNotes = Schema.Params
                .Where(p => !string.IsNullOrWhiteSpace(p.Note))
                .Select(p => $"{p.Name}: {p.Note}");
            var joined = string.Join(" · ", paramNotes);
            return joined.Length > 90 ? joined[..90] + "…" : joined;
        }
    }

    public string StageTitle => Schema.Stage.ToUpperInvariant();

    public PaletteModuleViewModel(PfxModuleSchema schema) => Schema = schema;
}
