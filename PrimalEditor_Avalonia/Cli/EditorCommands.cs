using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// Phase 0 seed commands (plan §10): the minimum surface that lets an agent
/// "看见" the running editor — liveness, schema introspection, scene query,
/// screenshot. Each write-verb added in later phases must come with a query
/// verb (§5.2) and description quality per §6.3.
/// </summary>
public sealed class EditorCommands
{
    [CliCommand("editor.ping",
        Description = "编辑器命令通道存活检查。返回进程信息、引擎加载状态、当前打开的项目与场景名。" +
                      "任何通道排障都从这里开始：连不上它等价于 exit code 5（编辑器未运行）。",
        Example = "{}")]
    public object Ping() => new
    {
        pong = true,
        pid = Environment.ProcessId,
        engineLoaded = Native.NativeEngineBridge.IsLoaded,
        engineInitialized = Native.NativeEngineBridge.IsEngineInitialized(),
        project = EditorServices.WorldEditor?.Project?.Name,
        scene = EditorServices.ActiveScene?.Name,
        uptimeSec = (int)EditorServices.Uptime.TotalSeconds,
        schemaVersion = CliHost.Registry.ComputeVersion(),
    };

    [CliCommand("editor.schema",
        Description = "返回全部命令的 JSON schema（含 $version 内容哈希、每条命令的参数表/示例/通道标注）。" +
                      "MCP tool 定义与 CI 快照门禁都以它为准。",
        Example = "{}")]
    public object Schema() => CliHost.Registry.BuildSchema();

    [CliCommand("scene.tree",
        Description = "序列化当前激活场景：场景 guid/名称 + 全部实体的 guid、名称、启用状态、组件清单。" +
                      "Transform 汇总为 position/rotation/scale（单位米/度，右手系，+Y 向上）。" +
                      "这是修改类命令的前置查询：先拿到 entityGuid 再调用实体/组件命令。" +
                      "无打开项目时返回 NOT_READY。",
        Example = "{}")]
    public object SceneTree()
    {
        var scene = EditorServices.ActiveScene
            ?? throw CliError.NotReady("当前没有激活场景",
                "先在编辑器中打开项目；用 editor.ping 确认 scene 字段非空");

        var entities = scene.Entities.Select(DescribeEntity).ToArray();
        return new
        {
            sceneGuid = scene.Guid.Value.ToString("D").ToLowerInvariant(),
            sceneName = scene.Name,
            isActive = scene.IsActive,
            isDirty = scene.IsDirty,
            entityCount = entities.Length,
            entities,
        };
    }

    // internal 供测试工程直接验证序列化形态（PrimalEditor_Avalonia.Tests 有 InternalsVisibleTo）。
    internal static object DescribeEntity(GameEntity entity) => new
    {
        entityGuid = entity.EntityGuid.Value.ToString("D").ToLowerInvariant(),
        name = entity.Name,
        isEnabled = entity.IsEnabled,
        parentGuid = entity.Parent?.EntityGuid.Value.ToString("D").ToLowerInvariant(),
        components = entity.Components.Select(DescribeComponent).ToArray(),
    };

    internal static object DescribeComponent(Component component)
    {
        if (component is Transform t)
        {
            return new
            {
                type = "Transform",
                position = new[] { t.PositionX, t.PositionY, t.PositionZ },
                rotationEuler = new[] { t.RotationX, t.RotationY, t.RotationZ },
                scale = new[] { t.ScaleX, t.ScaleY, t.ScaleZ },
            };
        }
        if (component is LightComponent light)
        {
            return new
            {
                type = "Light",
                lightType = light.LightType.ToString(),
                color = new[] { light.ColorR, light.ColorG, light.ColorB },
                intensity = light.Intensity,
                range = light.Range,
                castShadow = light.CastShadow,
            };
        }
        if (component is MeshRendererComponent mesh)
        {
            return new
            {
                type = "MeshRenderer",
                meshGuid = mesh.MeshGuid.Value == Guid.Empty ? null : mesh.MeshGuid.Value.ToString("D").ToLowerInvariant(),
                builtinMesh = mesh.BuiltinMesh.ToString(),
                castShadow = mesh.CastShadow,
            };
        }
        return new { type = component.GetType().Name };
    }

    [CliCommand("editor.screenshot",
        Description = "以编辑器视口相机渲染一帧并存为 PNG（复用视口现有离屏渲染目标，视口画面不受影响）。" +
                      "这是 Agent 的'眼睛'：修改后调用它做可视化验证。path 必须是绝对路径" +
                      "（相对路径按编辑器进程 cwd=可执行目录解析）；目录不存在会自动创建。" +
                      "3D 视口未初始化（无打开项目/窗口最小化前未渲染过）返回 NOT_READY。" +
                      "PNG 与视口同分辨率、RGBA16F → 8bit 色深转换，逐帧调用可构成看→改→验证闭环。",
        Example = """{"path":"/tmp/viewport.png"}""")]
    public object Screenshot(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw CliError.InvalidParams("path 不能为空", @"示例: {""path"":""/tmp/viewport.png""}");

        var viewport = EditorServices.FindViewport()
            ?? throw CliError.NotReady("3D 视口未就绪",
                "在编辑器中打开一个项目并切换到世界编辑器标签后再截图");

        var resolved = Path.IsPathRooted(path) ? path : Path.GetFullPath(path);
        var dir = Path.GetDirectoryName(resolved);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        if (!viewport.TryCaptureScreenshot(resolved, out var width, out var height))
            throw new CliError(CliErrorCodes.EngineError,
                "截图失败：视口尚未完成首次渲染或引擎忙",
                "先 engine 空 tick 等待 1-2 秒后重试，或用 editor.ping 确认 engineInitialized");

        return new
        {
            path = resolved,
            width,
            height,
            bytes = new FileInfo(resolved).Length,
        };
    }
}
