using System;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// sim.* 命令族（M3）：物理世界的播放控制与刚体查询。
/// 单位：米、秒；坐标：右手系 Y 上（与 Transform 一致）。
/// 前置：编辑器已打开场景；sim.play 前实体需有 Collider/RigidBody
///（Add-Component 面板或 sim.body.add）。
/// 失败语义：抛 CliError（NotReady/NotFound/InvalidParams）。
/// </summary>
public sealed class PhysicsCommands
{
    private static Avalonia.Threading.DispatcherTimer? _tickTimer;
    private static int _lastMaterialized;

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

    [CliCommand("sim.play",
        Description = "启动物理仿真（若未初始化则以缺省参数初始化物理世界）。此后引擎每帧以固定步长 1/60s 推进物理，并回写物理驱动实体的 Transform。无参数。",
        Example = """{}""")]
    public object Play()
    {
        RequireWorldEditor();
        if (!NativeEngineBridge.PhysicsIsInitialized())
        {
            if (!NativeEngineBridge.PhysicsInitialize())
                throw CliError.NotReady("物理世界初始化失败", "确认引擎已加载（editor.ping）");
            // 物理世界就绪后补物化：sim.body.add 可能发生在初始化之前（组件已入
            // ECS 但物化被跳过）——遍历全部存活实体补建物理体（幂等）
            _lastMaterialized = (int)NativeEngineBridge.PhysicsMaterializeAll();
        }
        NativeEngineBridge.PhysicsSetRunning(true);
        StartTickTimer();
        return new { running = true, tick_hz = 60, materialized_on_init = _lastMaterialized };
    }

    [CliCommand("sim.materialize",
        Description = "为所有带 Collider 的存活实体补建物理体（幂等；sim.play 会自动调用）。返回本次新建数量。调试用。",
        Example = """{}""")]
    public object Materialize()
    {
        RequireWorldEditor();
        if (!NativeEngineBridge.PhysicsIsInitialized())
            throw CliError.NotReady("物理世界未初始化", "先 sim.play 或确认引擎加载");
        var created = NativeEngineBridge.PhysicsMaterializeAll();
        return new { created };
    }

    [CliCommand("sim.pause",
        Description = "暂停物理仿真（保留当前状态；sim.step 仍可单步推进）。无参数。",
        Example = """{}""")]
    public object Pause()
    {
        RequireWorldEditor();
        StopTickTimer();
        NativeEngineBridge.PhysicsSetRunning(false);
        return new { running = false };
    }

    [CliCommand("sim.step",
        Description = "暂停状态下手动推进 N 个固定步长（每步 1/60s，N∈[1,600]）。单位：秒/60。隐含暂停语义：运行中先暂停再步进。失败：参数越界 → InvalidParams。",
        Example = """{"frames":1}""")]
    public object Step(int? frames = 1)
    {
        RequireWorldEditor();
        int n = Math.Clamp(frames ?? 1, 1, 600);
        NativeEngineBridge.PhysicsSetRunning(false);
        for (int i = 0; i < n; ++i) NativeEngineBridge.PipelineTick(1f / 60f);
        return new { stepped_frames = n, running = false };
    }

    [CliCommand("sim.status",
        Description = "查询物理世界状态：initialized（物理世界是否存在）、running（是否在推进）。无参数。",
        Example = """{}""")]
    public object Status()
    {
        return new
        {
            initialized = NativeEngineBridge.PhysicsIsInitialized(),
            running = NativeEngineBridge.PhysicsIsRunning(),
        };
    }

    [CliCommand("sim.body.add",
        Description = "为实体添加 Collider + RigidBody 组件并物化物理体。" +
            "shape: box|sphere（缺省 box）；mode: static|dynamic|kinematic（缺省 dynamic）；" +
            "half: box 半尺寸或 sphere 半径（米，'s' 或 'x,y,z'，缺省 0.5,0.5,0.5）；" +
            "mass 千克（缺省 1）；restitution/friction ∈[0,1]（缺省 0.1/0.5）。" +
            "失败：实体不存在 → NotFound，参数非法 → InvalidParams。",
        Example = """{"entityGuid":"0c9a1d2e-1111-2222-3333-444455556666","shape":"box","mode":"dynamic","mass":1.0}""")]
    public object BodyAdd(Guid entityGuid, string? shape = null, string? mode = null,
                           string? half = null, double? mass = null,
                           double? restitution = null, double? friction = null)
    {
        var entity = RequireEntity(entityGuid);

        var collider = new ColliderComponent();
        if (string.Equals(shape, "sphere", StringComparison.OrdinalIgnoreCase))
            collider.Shape = ColliderShape.Sphere;
        else if (!string.IsNullOrEmpty(shape) &&
                 !string.Equals(shape, "box", StringComparison.OrdinalIgnoreCase))
            throw CliError.InvalidParams($"shape '{shape}' 不支持（box|sphere）");
        if (!string.IsNullOrWhiteSpace(half))
        {
            var parts = half.Split(',');
            if (parts.Length == 1)
            {
                collider.HalfX = collider.HalfY = collider.HalfZ = float.Parse(parts[0]);
            }
            else if (parts.Length == 3)
            {
                collider.HalfX = float.Parse(parts[0]);
                collider.HalfY = float.Parse(parts[1]);
                collider.HalfZ = float.Parse(parts[2]);
            }
            else throw CliError.InvalidParams("half 应为 's' 或 'x,y,z'");
        }

        var rigidBody = new RigidBodyComponent();
        if (string.Equals(mode, "static", StringComparison.OrdinalIgnoreCase)) rigidBody.Mode = RigidBodyMode.Static;
        else if (string.Equals(mode, "kinematic", StringComparison.OrdinalIgnoreCase)) rigidBody.Mode = RigidBodyMode.Kinematic;
        else if (string.IsNullOrEmpty(mode) || string.Equals(mode, "dynamic", StringComparison.OrdinalIgnoreCase))
            rigidBody.Mode = RigidBodyMode.Dynamic;
        else throw CliError.InvalidParams($"mode '{mode}' 不支持（static|dynamic|kinematic）");
        if (mass.HasValue) rigidBody.Mass = (float)mass;
        if (restitution.HasValue) rigidBody.Restitution = (float)restitution;
        if (friction.HasValue) rigidBody.Friction = (float)friction;

        // 文档模型：组件进序列化路径；原生侧物化沿同步器语义（collider 先于 rigidbody）
        entity.Components.Add(collider);
        entity.Components.Add(rigidBody);
        var worldEditor = RequireWorldEditor();
        var nativeId = worldEditor.GetNativeEntityId(entity);
        if (nativeId != 0 && NativeEngineBridge.IsLoaded)
        {
            NativeEngineBridge.AddEntityCollider(nativeId,
                (uint)collider.Shape,
                new[] { collider.HalfX, collider.HalfY, collider.HalfZ },
                new[] { 0f, 0f, 0f },
                collider.IsTrigger,
                (uint)collider.Layer);
            NativeEngineBridge.AddEntityRigidBody(nativeId,
                (uint)rigidBody.Mode,
                rigidBody.Mass,
                rigidBody.Restitution,
                rigidBody.Friction,
                new[] { rigidBody.VelocityX, rigidBody.VelocityY, rigidBody.VelocityZ });
        }
        worldEditor.MarkProjectDirty();

        return new
        {
            entityGuid = entityGuid,
            collider = new { shape = collider.Shape.ToString(), half = $"{collider.HalfX},{collider.HalfY},{collider.HalfZ}" },
            rigid_body = new { mode = rigidBody.Mode.ToString(), mass = rigidBody.Mass },
        };
    }

    [CliCommand("sim.body.status",
        Description = "查询实体物理体的实时状态：position（米）、sleeping。" +
            "前置：实体已物化物理体（sim.body.add 或同步器推送）。" +
            "实体不存在 → NotFound；未物化 → materialized:false（非失败）。",
        Example = """{"entityGuid":"0c9a1d2e-1111-2222-3333-444455556666"}""")]
    public object BodyStatus(Guid entityGuid)
    {
        var entity = RequireEntity(entityGuid);
        var worldEditor = RequireWorldEditor();
        var nativeId = worldEditor.GetNativeEntityId(entity);
        var bodyId = nativeId != 0 ? NativeEngineBridge.PhysicsEntityBody(nativeId) : 0;
        if (bodyId == 0) return new { entityGuid = entityGuid, materialized = false };

        var t = NativeEngineBridge.PhysicsBodyGetTransform(bodyId);
        return new
        {
            entityGuid = entityGuid,
            materialized = true,
            position = t.HasValue ? new[] { t.Value.X, t.Value.Y, t.Value.Z } : Array.Empty<float>(),
            sleeping = t.HasValue && t.Value.Sleeping,
        };
    }

    // CLI 定时器：UI 线程 DispatcherTimer（Background 优先级）。PipelineTick 与
    // 视口渲染必须串行（ECS transform 无锁），线程池定时器会产生并发写——实测
    // 不能用 System.Timers.Timer。CLI 命令可能来自 socket 线程，故经
    // Dispatcher.UIThread.Post 派发到 UI 线程启动。
    private static void StartTickTimer()
    {
        Avalonia.Threading.Dispatcher.UIThread.Post(() =>
        {
            if (_tickTimer != null) { _tickTimer.Start(); return; }
            _tickTimer = new Avalonia.Threading.DispatcherTimer(
                TimeSpan.FromMilliseconds(16),
                Avalonia.Threading.DispatcherPriority.Background,
                (_, _) => NativeEngineBridge.PipelineTick(1f / 60f));
            _tickTimer.Start();
        }, Avalonia.Threading.DispatcherPriority.Send);
    }

    private static void StopTickTimer()
    {
        Avalonia.Threading.Dispatcher.UIThread.Post(() => _tickTimer?.Stop(),
            Avalonia.Threading.DispatcherPriority.Send);
    }
}
