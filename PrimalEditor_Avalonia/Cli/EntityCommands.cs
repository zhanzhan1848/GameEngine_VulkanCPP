using System;
using System.Collections.Generic;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Entities;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// Phase 1 编辑 verbs（CLI 规划 §10）：实体增删改、变换、灯光参数、undo。
/// 两条铁律：
///   1. 只动文档模型（Scene/GameEntity）再走 <see cref="SceneRuntimeSynchronizer"/>
///      同步到引擎——与 UI 拖拽/Inspector 完全同路径，绝不裸调 EngineDLL 制造第二事实源；
///   2. 每个修改都是一个 UndoRedo 动作（校验全过才落），editor.undo 与 Ctrl+Z
///      撤销的是同一个栈。
/// 所有 handler 由 CliHost 编组到 UI 线程执行。
/// </summary>
public sealed class EntityCommands
{
    // === 共享前置校验 ===

    private static WorldEditorViewModel RequireWorldEditor() =>
        EditorServices.WorldEditor
        ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");

    private static Scene RequireScene()
    {
        var scene = RequireWorldEditor().ActiveScene
            ?? throw CliError.NotReady("当前没有激活场景",
                "先在编辑器中打开项目；用 editor.ping 确认 scene 字段非空");
        return scene;
    }

    private static GameEntity RequireEntity(Guid entityGuid)
    {
        if (entityGuid == Guid.Empty)
            throw CliError.InvalidParams("entityGuid 不能为空 Guid",
                "先用 scene.tree 查询有效 guid");
        var scene = RequireScene();
        var entity = FindEntity(scene.Entities, entityGuid)
            ?? throw CliError.NotFound(
                $"实体 {entityGuid.ToString().ToLowerInvariant()}",
                "先用 scene.tree 查询有效 guid（实体可能已被删除，或属于其他场景）");
        return entity;
    }

    private static GameEntity? FindEntity(IEnumerable<GameEntity> entities, Guid guid)
    {
        foreach (var entity in entities)
        {
            if (entity.EntityGuid.Value == guid) return entity;
            var inChildren = FindEntity(entity.Children, guid);
            if (inChildren != null) return inChildren;
        }
        return null;
    }

    // === 查询 ===

    [CliCommand("entity.get",
        Description = "按 guid 查询单个实体：名称、启用状态、父级、组件明细（同 scene.tree 的实体条目形态）。" +
                      "修改类命令的前置查询。实体不存在返回 NOT_FOUND。",
        Example = """{"entityGuid":"0c9e6c9a-2e5f-4a5b-8b1e-3f2a4d5c6b7a"}""")]
    public object Get(Guid entityGuid)
    {
        var entity = RequireEntity(entityGuid);
        return EditorCommands.DescribeEntity(entity);
    }

    // === 实体增删改 ===

    [CliCommand("entity.create",
        Description = "从工厂创建实体并放入当前场景根。kind 取值见 EntityKind：" +
                      "Empty/Character/Prop/Sphere/Box/Plane/Light/DirectionalLight/EnvironmentLight/Camera。" +
                      "name/position/rotationEuler/scale 可选，创建前注入（一次 undo 即整体撤销）。" +
                      "单位米、旋转为欧拉角（度）、右手系 +Y 向上。创建后视口立即可见。" +
                      "返回新实体（含 entityGuid）。无激活场景返回 NOT_READY。",
        Example = """{"kind":"Sphere","name":"我的球","position":[0,1,0]}""")]
    public object Create(EntityKind kind, string? name = null,
        Vec3? position = null, Vec3? rotationEuler = null, Vec3? scale = null)
    {
        var descriptor = EntityFactory.Find(kind)
            ?? throw CliError.InvalidParams($"未知的实体类型 '{kind}'",
                "有效取值: " + string.Join("/", Enum.GetNames<EntityKind>()));
        var worldEditor = RequireWorldEditor();
        var scene = RequireScene();

        var entity = descriptor.CreateEntity();
        if (!string.IsNullOrWhiteSpace(name)) entity.Name = name!;
        ApplyOptionalTransform(entity, position, rotationEuler, scale);

        // 与 WorldEditorViewModel.CreateEntityFromFactory 相同的落库序列，
        // 但 name/transform 在入栈前注入 → 一次 undo 整体撤销。
        worldEditor.AddEntity(entity);
        worldEditor.UndoRedo.Add(new AddEntityAction(worldEditor, entity));
        worldEditor.SelectedEntities = new[] { entity };
        worldEditor.MarkProjectDirty();

        return EditorCommands.DescribeEntity(entity);
    }

    private static void ApplyOptionalTransform(GameEntity entity,
        Vec3? position, Vec3? rotationEuler, Vec3? scale)
    {
        var transform = entity.GetTransform();
        if (transform == null) return;
        if (position is { } p) { transform.PositionX = p.X; transform.PositionY = p.Y; transform.PositionZ = p.Z; }
        if (rotationEuler is { } r) { transform.RotationX = r.X; transform.RotationY = r.Y; transform.RotationZ = r.Z; }
        if (scale is { } s)
        {
            if (s.X <= 0 || s.Y <= 0 || s.Z <= 0)
                throw CliError.InvalidParams("scale 分量必须 > 0", "示例: [1,1,1]");
            transform.ScaleX = s.X; transform.ScaleY = s.Y; transform.ScaleZ = s.Z;
        }
    }

    [CliCommand("entity.delete",
        Description = "按 guid 删除实体（含其组件的原生同步移除），记入 undo 栈。删除选中实体会同步清空选择。" +
                      "guid 无效返回 NOT_FOUND。",
        Example = """{"entityGuid":"0c9e6c9a-2e5f-4a5b-8b1e-3f2a4d5c6b7a"}""")]
    public object Delete(Guid entityGuid)
    {
        var entity = RequireEntity(entityGuid);
        var worldEditor = RequireWorldEditor();

        using (worldEditor.UndoRedo.BeginTransaction($"CLI Delete {entity.Name}"))
        {
            var action = new RemoveEntityAction(worldEditor, entity);
            worldEditor.UndoRedo.Add(action);
            action.Redo();
        }
        if (worldEditor.SelectedEntities.Contains(entity))
        {
            worldEditor.SelectedEntities =
                worldEditor.SelectedEntities.Where(e => e != entity).ToArray();
        }
        return new { deleted = entity.EntityGuid.Value.ToString("D").ToLowerInvariant(), name = entity.Name };
    }

    [CliCommand("entity.duplicate",
        Description = "按 guid 复制实体（命名追加 _Copy），复制体与本体同位置，返回新实体。" +
                      "与编辑器 Ctrl+D 走同一动作类：目前克隆 Transform 与启用状态，" +
                      "灯光/网格组件的克隆与 UI 行为一致地逐步补齐。",
        Example = """{"entityGuid":"0c9e6c9a-2e5f-4a5b-8b1e-3f2a4d5c6b7a"}""")]
    public object Duplicate(Guid entityGuid)
    {
        var original = RequireEntity(entityGuid);
        var worldEditor = RequireWorldEditor();
        var scene = RequireScene();

        var before = scene.Entities.ToHashSet();
        using (worldEditor.UndoRedo.BeginTransaction($"CLI Duplicate {original.Name}"))
        {
            var action = new DuplicateEntityAction(worldEditor, original);
            worldEditor.UndoRedo.Add(action);
            action.Redo();
        }
        var clone = scene.Entities.FirstOrDefault(e => !before.Contains(e))
            ?? throw new CliError(CliErrorCodes.Internal, "复制后未找到新实体", "用 scene.tree 核对场景状态后重试");
        return EditorCommands.DescribeEntity(clone);
    }

    [CliCommand("entity.set-transform",
        Description = "按 guid 设置实体变换。position/rotationEuler/scale 均为可选——只传的字段生效" +
                      "（每个字段是完整的三元组，不支持单分量修改）。单位米/度，右手系 +Y 向上。" +
                      "修改后视口即时刷新并推送到引擎。记入 undo。scale 分量必须 > 0。",
        Example = """{"entityGuid":"…","position":[1,0,3],"rotationEuler":[0,90,0]}""")]
    public object SetTransform(Guid entityGuid, Vec3? position = null, Vec3? rotationEuler = null, Vec3? scale = null)
    {
        if (position == null && rotationEuler == null && scale == null)
            throw CliError.InvalidParams("至少提供 position/rotationEuler/scale 之一",
                "示例: {\"position\":[1,0,3]}");

        var entity = RequireEntity(entityGuid);
        var transform = entity.GetTransform()
            ?? throw CliError.NotFound($"实体 '{entity.Name}' 没有 Transform 组件",
                "scene.tree 查看该实体的组件列表");
        if (scale is { } s && (s.X <= 0 || s.Y <= 0 || s.Z <= 0))
            throw CliError.InvalidParams("scale 分量必须 > 0", "示例: [1,1,1]");

        var worldEditor = RequireWorldEditor();
        var oldData = TransformData.FromTransform(transform);
        var newData = oldData;
        if (position is { } p) { newData.PositionX = p.X; newData.PositionY = p.Y; newData.PositionZ = p.Z; }
        if (rotationEuler is { } r) { newData.RotationX = r.X; newData.RotationY = r.Y; newData.RotationZ = r.Z; }
        if (scale is { } sc) { newData.ScaleX = sc.X; newData.ScaleY = sc.Y; newData.ScaleZ = sc.Z; }

        // SyncedTransformAction 在 Undo/Redo 两个方向都推送原生，
        // 避免 CLI 改完、用户 Ctrl+Z 后引擎残留旧值的漂移。
        worldEditor.UndoRedo.Add(new SyncedTransformAction(worldEditor, entity, transform, oldData, newData));
        newData.ApplyTo(transform);
        worldEditor.PushTransformsToNative(new[] { entity });
        worldEditor.MarkProjectDirty();

        return new
        {
            entityGuid = entityGuid.ToString("D").ToLowerInvariant(),
            position = new[] { transform.PositionX, transform.PositionY, transform.PositionZ },
            rotationEuler = new[] { transform.RotationX, transform.RotationY, transform.RotationZ },
            scale = new[] { transform.ScaleX, transform.ScaleY, transform.ScaleZ },
        };
    }

    private sealed class SyncedTransformAction : IUndoRedoAction
    {
        private readonly WorldEditorViewModel _worldEditor;
        private readonly GameEntity _entity;
        private readonly Transform _transform;
        private readonly TransformData _oldData;
        private readonly TransformData _newData;

        public string Name => "CLI Transform Edit";

        public SyncedTransformAction(WorldEditorViewModel worldEditor, GameEntity entity,
            Transform transform, TransformData oldData, TransformData newData)
        {
            _worldEditor = worldEditor; _entity = entity;
            _transform = transform; _oldData = oldData; _newData = newData;
        }

        public void Undo()
        {
            _oldData.ApplyTo(_transform);
            _worldEditor.PushTransformsToNative(new[] { _entity });
        }

        public void Redo()
        {
            _newData.ApplyTo(_transform);
            _worldEditor.PushTransformsToNative(new[] { _entity });
        }
    }

    [CliCommand("entity.set-name",
        Description = "按 guid 重命名实体。名字只存在于编辑器文档（不进引擎）。记入 undo。" +
                      "空名字返回 INVALID_PARAMS。",
        Example = """{"entityGuid":"…","name":"主门"}""")]
    public object SetName(Guid entityGuid, string name)
    {
        if (string.IsNullOrWhiteSpace(name))
            throw CliError.InvalidParams("name 不能为空", "示例: {\"name\":\"主门\"}");
        var entity = RequireEntity(entityGuid);
        var worldEditor = RequireWorldEditor();

        var oldName = entity.Name;
        worldEditor.UndoRedo.Add(new RenameEntityAction(entity, name));
        entity.Name = name;
        worldEditor.MarkProjectDirty();
        return new { entityGuid = entityGuid.ToString("D").ToLowerInvariant(), oldName, newName = name };
    }

    // === 灯光 ===

    [CliCommand("light.set",
        Description = "按 guid 修改实体上的 Light 组件参数。所有字段可选——只传的字段生效。" +
                      "lightType 取值 Directional/Point/Spot；intensity/range 必须 ≥ 0；color 为 RGB 线性值 0..1。" +
                      "修改后立即推送引擎（与 Inspector 编辑同路径），记入 undo。" +
                      "实体没有 Light 组件返回 NOT_FOUND（先 entity.create kind=Light/DirectionalLight）。",
        Example = """{"entityGuid":"…","intensity":8,"color":[1,0.5,0.2]}""")]
    public object SetLight(Guid entityGuid, LightType? lightType = null, float? intensity = null,
        Vec3? color = null, float? range = null, bool? castShadow = null, bool? isEnabled = null)
    {
        if (lightType == null && intensity == null && color == null && range == null
            && castShadow == null && isEnabled == null)
            throw CliError.InvalidParams("至少提供 lightType/intensity/color/range/castShadow/isEnabled 之一",
                "示例: {\"intensity\":8,\"color\":[1,0.5,0.2]}");
        if (intensity is { } i && i < 0) throw CliError.InvalidParams("intensity 必须 ≥ 0");
        if (range is { } rg && rg < 0) throw CliError.InvalidParams("range 必须 ≥ 0");
        if (color is { } c && (c.X < 0 || c.Y < 0 || c.Z < 0 || c.X > 1 || c.Y > 1 || c.Z > 1))
            throw CliError.InvalidParams("color 分量必须在 0..1", "线性 RGB，示例: [1,0.5,0.2]");

        var entity = RequireEntity(entityGuid);
        var light = entity.GetComponent<LightComponent>()
            ?? throw CliError.NotFound($"实体 '{entity.Name}' 没有 Light 组件",
                "scene.tree 查看组件；用 entity.create kind=Light 先创建灯光实体");

        var worldEditor = RequireWorldEditor();
        var old = LightSnapshot.From(light);
        var newData = old with
        {
            LightType = lightType ?? old.LightType,
            Intensity = intensity ?? old.Intensity,
            ColorR = color?.X ?? old.ColorR,
            ColorG = color?.Y ?? old.ColorG,
            ColorB = color?.Z ?? old.ColorB,
            Range = range ?? old.Range,
            CastShadow = castShadow ?? old.CastShadow,
            IsEnabled = isEnabled ?? old.IsEnabled,
        };

        worldEditor.UndoRedo.Add(new DelegateActionLight(worldEditor, entity, light, old, newData));
        newData.ApplyTo(light);
        PushLight(worldEditor, entity, light);
        worldEditor.MarkProjectDirty();

        return new
        {
            entityGuid = entityGuid.ToString("D").ToLowerInvariant(),
            lightType = light.LightType.ToString(),
            color = new[] { light.ColorR, light.ColorG, light.ColorB },
            intensity = light.Intensity,
            range = light.Range,
            castShadow = light.CastShadow,
            isEnabled = light.IsEnabled,
        };
    }

    private static void PushLight(WorldEditorViewModel worldEditor, GameEntity entity, LightComponent light)
    {
        if (!NativeEngineBridge.IsLoaded) return;
        var nativeId = worldEditor.GetNativeEntityId(entity);
        if (nativeId == 0) return; // 实体尚未同步到引擎（引擎未加载/同步器未跑）——文档已改，引擎侧等 Sync
        NativeEngineBridge.SetEntityLight(nativeId,
            (uint)light.LightType,
            light.Intensity,
            new[] { light.ColorR, light.ColorG, light.ColorB },
            light.Range,
            light.UmbraAngle,
            light.PenumbraAngle,
            light.IsEnabled);
    }

    private sealed record LightSnapshot
    {
        public LightType LightType { get; init; }
        public float ColorR { get; init; }
        public float ColorG { get; init; }
        public float ColorB { get; init; }
        public float Intensity { get; init; }
        public float Range { get; init; }
        public bool CastShadow { get; init; }
        public bool IsEnabled { get; init; }

        public static LightSnapshot From(LightComponent l) => new()
        {
            LightType = l.LightType,
            ColorR = l.ColorR, ColorG = l.ColorG, ColorB = l.ColorB,
            Intensity = l.Intensity,
            Range = l.Range,
            CastShadow = l.CastShadow,
            IsEnabled = l.IsEnabled,
        };

        public void ApplyTo(LightComponent l)
        {
            l.LightType = LightType;
            l.ColorR = ColorR; l.ColorG = ColorG; l.ColorB = ColorB;
            l.Intensity = Intensity;
            l.Range = Range;
            l.CastShadow = CastShadow;
            l.IsEnabled = IsEnabled;
        }
    }

    // undo/redo 两个方向都重推引擎，避免撤销后引擎残留新值的漂移。
    private sealed class DelegateActionLight : IUndoRedoAction
    {
        private readonly WorldEditorViewModel _worldEditor;
        private readonly GameEntity _entity;
        private readonly LightComponent _light;
        private readonly LightSnapshot _oldData;
        private readonly LightSnapshot _newData;

        public string Name => "CLI Light Edit";

        public DelegateActionLight(WorldEditorViewModel worldEditor, GameEntity entity,
            LightComponent light, LightSnapshot oldData, LightSnapshot newData)
        {
            _worldEditor = worldEditor; _entity = entity; _light = light;
            _oldData = oldData; _newData = newData;
        }

        public void Undo()
        {
            _oldData.ApplyTo(_light);
            PushLight(_worldEditor, _entity, _light);
        }

        public void Redo()
        {
            _newData.ApplyTo(_light);
            PushLight(_worldEditor, _entity, _light);
        }
    }

    // === 执行控制 ===

    [CliCommand("editor.undo",
        Description = "撤销最近一次编辑（与编辑器 Ctrl+Z 同一 undo 栈，含 UI 与 CLI 的全部操作）。" +
                      "栈空时 ok=true 且 undone=false（不算错误）。",
        Example = "{}")]
    public object Undo()
    {
        var worldEditor = RequireWorldEditor();
        if (worldEditor.UndoRedo.UndoStack.Count == 0)
            return new { undone = false, reason = "undo 栈为空" };
        var name = worldEditor.UndoRedo.UndoStack[^1].Name;
        worldEditor.UndoRedo.Undo();
        return new
        {
            undone = true,
            undoneAction = name,
            undoDepth = worldEditor.UndoRedo.UndoStack.Count,
            redoDepth = worldEditor.UndoRedo.RedoStack.Count,
        };
    }

    [CliCommand("editor.redo",
        Description = "重做上一次被撤销的编辑（与编辑器 Ctrl+Y/Ctrl+Shift+Z 同一栈）。",
        Example = "{}")]
    public object Redo()
    {
        var worldEditor = RequireWorldEditor();
        if (worldEditor.UndoRedo.RedoStack.Count == 0)
            return new { redone = false, reason = "redo 栈为空" };
        var name = worldEditor.UndoRedo.RedoStack[^1].Name;
        worldEditor.UndoRedo.Redo();
        return new
        {
            redone = true,
            redoneAction = name,
            undoDepth = worldEditor.UndoRedo.UndoStack.Count,
            redoDepth = worldEditor.UndoRedo.RedoStack.Count,
        };
    }

    [CliCommand("engine.flush",
        Description = "阻塞直到 GPU 队列排空（WaitIdle）。截图/读回前需要严格时序时使用；" +
                      "editor.screenshot 内部已自带 flush。",
        Example = "{}")]
    public object Flush()
    {
        if (!NativeEngineBridge.IsLoaded)
            throw CliError.NotReady("引擎未加载", "用 editor.ping 查看 engineLoaded");
        NativeEngineBridge.FlushGPU();
        return new { flushed = true };
    }

    [CliCommand("engine.tick",
        Description = "泵动编辑器的引擎作业（异步资产上传等）N 次，返回剩余待上传数。" +
                      "编辑器的渲染是连续的（无需手动 tick 推进画面）；本命令用于加速后台作业消化" +
                      "或在截图前确保资产就位。frames 上限 240。",
        Example = """{"frames":10}""")]
    public object Tick(int frames = 1)
    {
        if (frames < 1) throw CliError.InvalidParams("frames 必须 ≥ 1");
        if (frames > 240) throw CliError.InvalidParams("frames 上限 240（编辑器通道不需要大批量 tick）");
        if (!NativeEngineBridge.IsLoaded)
            throw CliError.NotReady("引擎未加载", "用 editor.ping 查看 engineLoaded");
        for (int i = 0; i < frames; i++)
            NativeEngineBridge.TickAssetSystem();
        return new { ticks = frames, pendingUploads = (long)NativeEngineBridge.GetPendingUploadCount() };
    }
}
