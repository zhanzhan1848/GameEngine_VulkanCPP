using System;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// 组件级编辑 verbs（CLI 规划 §5.1 editor 通道）：camera.set / material.set-param。
/// camera.set 只动文档模型（与 Inspector 一致，场景同步时生效——编辑器现状相机参数
/// 不做逐字段热推）；material.set-param 走 EngineDLL 导出直接命中引擎侧材质
/// （与 §12.2 边界纪律一致：修改永远走导出 + 注册表，不发明第二通道）。
/// </summary>
public sealed class ComponentCommands
{
    private static WorldEditorViewModel RequireWorldEditor() =>
        EditorServices.WorldEditor
        ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");

    private static GameEntity RequireEntity(Guid entityGuid)
    {
        var scene = EditorServices.ActiveScene
            ?? throw CliError.NotReady("当前没有激活场景", "先在编辑器中打开项目");
        var entity = FindEntity(scene.Entities, entityGuid)
            ?? throw CliError.NotFound(
                $"实体 {entityGuid.ToString().ToLowerInvariant()}",
                "先用 scene.tree 查询有效 guid（实体可能已被删除）");
        return entity;
    }

    private static GameEntity? FindEntity(System.Collections.Generic.IEnumerable<GameEntity> entities, Guid guid)
    {
        foreach (var entity in entities)
        {
            if (entity.EntityGuid.Value == guid) return entity;
            var inChildren = FindEntity(entity.Children, guid);
            if (inChildren != null) return inChildren;
        }
        return null;
    }

    // === 相机 ===

    [CliCommand("camera.set",
        Description = "修改实体上的 Camera 组件参数，全部字段可选、只传的生效。" +
                      "projection 取值 Perspective/Orthographic；fov 单位度（0<fov<180）；" +
                      "orthoSize/nearPlane/farPlane 必须 > 0 且 near<far；clearColor 为 RGBA 线性 0..1。" +
                      "与 Inspector 编辑同路径：先落文档模型（记入 undo），场景同步时推送引擎" +
                      "（编辑器视口取景用的是编辑器相机，本组件影响游戏/运行视角）。" +
                      "实体没有 Camera 组件返回 NOT_FOUND。",
        Example = """{"entityGuid":"…","fov":45,"nearPlane":0.1,"farPlane":500}""")]
    public object SetCamera(Guid entityGuid, string? projection = null, float? fov = null,
        float? orthoSize = null, float? nearPlane = null, float? farPlane = null, Vec3? clearColor = null)
    {
        if (projection == null && fov == null && orthoSize == null && nearPlane == null
            && farPlane == null && clearColor == null)
            throw CliError.InvalidParams("至少提供 projection/fov/orthoSize/nearPlane/farPlane/clearColor 之一",
                @"示例: {""fov"":45}");

        var entity = RequireEntity(entityGuid);
        var camera = entity.GetComponent<CameraComponent>()
            ?? throw CliError.NotFound($"实体 '{entity.Name}' 没有 Camera 组件",
                "用 entity.create kind=Camera 先创建相机实体");

        // 先整体校验（fail-fast，校验全过才落——§10 Phase 1 原子性要求）。
        var newProjection = camera.Projection;
        if (projection != null)
        {
            newProjection = ParseProjection(projection!);
        }
        if (fov is { } f && (f <= 0 || f >= 180))
            throw CliError.InvalidParams("fov 必须在 0..180 开区间内（单位度）", @"示例: 60");
        if (orthoSize is { } os && os <= 0)
            throw CliError.InvalidParams("orthoSize 必须 > 0", @"示例: 10");
        var newNear = nearPlane ?? camera.NearPlane;
        var newFar = farPlane ?? camera.FarPlane;
        if (newNear <= 0) throw CliError.InvalidParams("nearPlane 必须 > 0", @"示例: 0.1");
        if (newFar <= newNear)
            throw CliError.InvalidParams($"farPlane({newFar}) 必须 > nearPlane({newNear})",
                @"示例: {""nearPlane"":0.1,""farPlane"":500}");
        if (clearColor is { } c && (c.X < 0 || c.Y < 0 || c.Z < 0 || c.X > 1 || c.Y > 1 || c.Z > 1))
            throw CliError.InvalidParams("clearColor 各分量必须在 0..1（clearColor 的 A 分量固定为 1）",
                "线性 RGB，示例: [0.2,0.3,0.4]");

        var worldEditor = RequireWorldEditor();
        var old = CameraSnapshot.From(camera);
        var newData = new CameraSnapshot
        {
            Projection = newProjection,
            FieldOfViewDegrees = fov ?? camera.FieldOfViewDegrees,
            OrthographicSize = orthoSize ?? camera.OrthographicSize,
            NearPlane = newNear,
            FarPlane = newFar,
            ClearColorR = clearColor?.X ?? camera.ClearColorR,
            ClearColorG = clearColor?.Y ?? camera.ClearColorG,
            ClearColorB = clearColor?.Z ?? camera.ClearColorB,
            ClearColorA = camera.ClearColorA,
        };

        worldEditor.UndoRedo.Add(new DelegateActionCamera(camera, old, newData));
        newData.ApplyTo(camera);
        worldEditor.MarkProjectDirty();

        return new
        {
            entityGuid = entityGuid.ToString("D").ToLowerInvariant(),
            projection = camera.Projection.ToString(),
            fov = camera.FieldOfViewDegrees,
            orthoSize = camera.OrthographicSize,
            nearPlane = camera.NearPlane,
            farPlane = camera.FarPlane,
            clearColor = new[] { camera.ClearColorR, camera.ClearColorG, camera.ClearColorB, camera.ClearColorA },
        };
    }

    private static CameraProjection ParseProjection(string value)
    {
        foreach (var name in Enum.GetNames<CameraProjection>())
        {
            if (name.Equals(value, StringComparison.OrdinalIgnoreCase))
                return Enum.Parse<CameraProjection>(name, ignoreCase: true);
        }
        throw CliError.InvalidParams($"未知的 projection '{value}'",
            "有效取值: " + string.Join("/", Enum.GetNames<CameraProjection>()));
    }

    private sealed record CameraSnapshot
    {
        public CameraProjection Projection { get; init; }
        public float FieldOfViewDegrees { get; init; }
        public float OrthographicSize { get; init; }
        public float NearPlane { get; init; }
        public float FarPlane { get; init; }
        public float ClearColorR { get; init; }
        public float ClearColorG { get; init; }
        public float ClearColorB { get; init; }
        public float ClearColorA { get; init; }

        public static CameraSnapshot From(CameraComponent c) => new()
        {
            Projection = c.Projection,
            FieldOfViewDegrees = c.FieldOfViewDegrees,
            OrthographicSize = c.OrthographicSize,
            NearPlane = c.NearPlane,
            FarPlane = c.FarPlane,
            ClearColorR = c.ClearColorR, ClearColorG = c.ClearColorG,
            ClearColorB = c.ClearColorB, ClearColorA = c.ClearColorA,
        };

        public void ApplyTo(CameraComponent c)
        {
            // 组件 setter 强制 near<far 不变量：跨值修改时先动远端，避免中间态抛异常。
            if (NearPlane >= c.FarPlane)
            {
                c.FarPlane = FarPlane;
                c.NearPlane = NearPlane;
            }
            else
            {
                c.NearPlane = NearPlane;
                c.FarPlane = FarPlane;
            }
            c.Projection = Projection;
            c.FieldOfViewDegrees = FieldOfViewDegrees;
            c.OrthographicSize = OrthographicSize;
            c.ClearColorR = ClearColorR; c.ClearColorG = ClearColorG;
            c.ClearColorB = ClearColorB; c.ClearColorA = ClearColorA;
        }
    }

    /// <summary>相机组件无逐字段原生推送导出（编辑器现状：Inspector 编辑同样只落文档，
    /// 场景同步时整体 AddEntityCamera）。Undo/Redo 与 UI 保持一致地只动文档。</summary>
    private sealed class DelegateActionCamera : IUndoRedoAction
    {
        private readonly CameraComponent _camera;
        private readonly CameraSnapshot _oldData;
        private readonly CameraSnapshot _newData;

        public string Name => "CLI Camera Edit";

        public DelegateActionCamera(CameraComponent camera, CameraSnapshot oldData, CameraSnapshot newData)
        {
            _camera = camera; _oldData = oldData; _newData = newData;
        }

        public void Undo() => _oldData.ApplyTo(_camera);
        public void Redo() => _newData.ApplyTo(_camera);
    }

    // === 材质参数 ===

    [CliCommand("material.set-param",
        Description = "直接设置引擎侧该实体材质的 PBR 参数（走 EngineDLL 导出，与 C# 材质面板同通道）。" +
                      "roughness/metallic/alphaCutoff 取值 0..1；baseColor 为 RGBA 线性 0..1；" +
                      "technique 为着色技术索引（0=默认 PBR）。全部字段可选、只传的生效。" +
                      "这是引擎侧覆盖：不进 undo 栈（重开场景/重新同步后回落到材质资产定义值）。" +
                      "实体尚未同步到引擎或没有可渲染材质时返回 NOT_FOUND。",
        Example = """{"entityGuid":"…","roughness":0.2,"metallic":1.0}""")]
    public object SetMaterialParam(Guid entityGuid, float? roughness = null, float? metallic = null,
        float? alphaCutoff = null, Vec3? baseColor = null, float? baseColorA = null, int? technique = null)
    {
        if (roughness == null && metallic == null && alphaCutoff == null && baseColor == null && technique == null)
            throw CliError.InvalidParams("至少提供 roughness/metallic/alphaCutoff/baseColor/technique 之一",
                @"示例: {""roughness"":0.2,""metallic"":1.0}");
        if (roughness is { } r && (r < 0 || r > 1)) throw CliError.InvalidParams("roughness 必须在 0..1");
        if (metallic is { } m && (m < 0 || m > 1)) throw CliError.InvalidParams("metallic 必须在 0..1");
        if (alphaCutoff is { } a && (a < 0 || a > 1)) throw CliError.InvalidParams("alphaCutoff 必须在 0..1");
        if (baseColor is { } c && (c.X < 0 || c.Y < 0 || c.Z < 0 || c.X > 1 || c.Y > 1 || c.Z > 1))
            throw CliError.InvalidParams("baseColor 各分量必须在 0..1", "线性 RGB，示例: [0.8,0.1,0.1]");
        if (baseColorA is { } ca && baseColor == null)
            throw CliError.InvalidParams("baseColorA 必须与 baseColor 一起提供", @"示例: {""baseColor"":[0.8,0.1,0.1],""baseColorA"":1}");
        if (technique is { } t && t < 0) throw CliError.InvalidParams("technique 必须 ≥ 0");

        var entity = RequireEntity(entityGuid);
        var worldEditor = RequireWorldEditor();
        if (!NativeEngineBridge.IsLoaded)
            throw CliError.NotReady("引擎未加载", "用 editor.ping 查看 engineLoaded");
        var nativeId = worldEditor.GetNativeEntityId(entity);
        if (nativeId == 0)
            throw CliError.NotFound($"实体 '{entity.Name}' 尚未同步到引擎（无原生实体 id）",
                "引擎加载后重试；实体是 CLI 新建时先 engine.tick 1 等同步器消化");

        if (roughness is { } r2) NativeEngineBridge.SetMaterialRoughness(nativeId, r2);
        if (metallic is { } m2) NativeEngineBridge.SetMaterialMetallic(nativeId, m2);
        if (alphaCutoff is { } a2) NativeEngineBridge.SetMaterialAlphaCutoff(nativeId, a2);
        if (baseColor is { } c2)
            NativeEngineBridge.SetMaterialBaseColor(nativeId, c2.X, c2.Y, c2.Z, baseColorA ?? 1f);
        if (technique is { } t2) NativeEngineBridge.SetMaterialTechnique(nativeId, (uint)t2);

        return new
        {
            entityGuid = entityGuid.ToString("D").ToLowerInvariant(),
            applied = new
            {
                roughness, metallic, alphaCutoff,
                baseColor = baseColor is { } bc ? new[] { bc.X, bc.Y, bc.Z, baseColorA ?? 1f } : null,
                technique,
            },
            hint = "引擎侧覆盖不持久化；用 editor.screenshot 验证效果",
        };
    }
}
