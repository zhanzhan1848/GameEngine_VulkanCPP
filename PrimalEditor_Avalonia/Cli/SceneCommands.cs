using System;
using System.IO;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// 场景与资产查询 verbs（CLI 规划 §5.1 editor 通道）：scene.new / scene.save /
/// scene.load / asset.list。场景结构操作与 UI 菜单走同一 API（AddScene /
/// EditorSaveService / Serializer），激活场景的引擎同步由 ActiveScene setter
/// 内的 SceneRuntimeSynchronizer.Sync 统一完成。
/// </summary>
public sealed class SceneCommands
{
    private static WorldEditorViewModel RequireWorldEditor() =>
        EditorServices.WorldEditor
        ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");

    private static Project RequireProject() =>
        RequireWorldEditor().Project
        ?? throw CliError.NotReady("当前没有打开的项目", "先在编辑器中打开项目（editor.ping 查看 project 字段）");

    private static Scene RequireActiveScene() =>
        EditorServices.ActiveScene
        ?? throw CliError.NotReady("当前没有激活场景", "先 scene.load 或 scene.new，再用 editor.ping 确认 scene 字段");

    [CliCommand("scene.new",
        Description = "在当前项目中新建场景并激活（激活即触发引擎全量同步，与 UI 新建场景一致）。" +
                      "新场景自带一盏默认平行光。name 缺省为 'New Scene'。记入项目（保存后才落盘，" +
                      "用 scene.save 持久化）。",
        Example = """{"name":"夜城"}""")]
    public object New(string? name = null)
    {
        var worldEditor = RequireWorldEditor();
        RequireProject();
        var finalName = string.IsNullOrWhiteSpace(name) ? "New Scene" : name!.Trim();
        worldEditor.AddScene(finalName);
        var scene = worldEditor.ActiveScene!;
        return DescribeScene(scene, created: true);
    }

    [CliCommand("scene.save",
        Description = "保存当前项目与全部场景到磁盘（与编辑器 Ctrl+S 的 EditorSaveService.SaveAll 同路径）。" +
                      "返回项目文件与每个场景的落盘路径。没有打开项目返回 NOT_READY。",
        Example = "{}")]
    public object Save()
    {
        var project = RequireProject();
        var ok = EditorSaveService.Instance.SaveAll(project);
        if (!ok)
            throw new CliError(CliErrorCodes.EngineError,
                "保存失败：" + string.Join("; ", EditorSaveService.LastSaveAllFailures),
                "检查磁盘空间与项目目录写权限后重试");
        return new
        {
            saved = true,
            projectFile = project.Path,
            scenes = project.Scenes.Select(s => Serializer.ScenePath(project, s.Guid)).ToArray(),
        };
    }

    [CliCommand("scene.load",
        Description = "把项目内已有的 .scene 场景设为激活（guid 命中当前项目已打开的场景时直接切换）。" +
                      "path 相对项目 Content 根目录解析，也可以给绝对路径；文件不存在返回 NOT_FOUND。" +
                      "激活触发引擎全量同步，视口随即显示新场景。新场景文件请用 scene.new + scene.save 制作。",
        Example = """{"path":"Scenes/9f2c8e41-…-.scene"}""")]
    public object Load(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw CliError.InvalidParams("path 不能为空",
                @"用 scene.save 的返回值或 asset.list 查看可用场景路径");
        var worldEditor = RequireWorldEditor();
        var project = RequireProject();

        var resolved = ResolveScenePath(project, path!);
        if (!File.Exists(resolved))
            throw CliError.NotFound($"场景文件 '{path}'", "项目内可用 .scene 文件见 " +
                Path.Combine(project.Path, "Scenes") + "（按 guid 命名）");

        Scene? loaded;
        try
        {
            loaded = Serializer.LoadScene(resolved);
        }
        catch (Exception ex)
        {
            throw new CliError(CliErrorCodes.EngineError, $"场景文件解析失败: {ex.Message}",
                "确认文件是编辑器保存的 .scene JSON（scene.save 生成）");
        }
        if (loaded == null)
            throw new CliError(CliErrorCodes.EngineError, $"场景文件无法读取: {resolved}",
                "确认文件未被其他进程锁定后重试");

        // guid 与已打开场景相同 → 就地换内容后保持引用稳定（与 LoadProjectScenes 同策略）。
        var existing = worldEditor.Scenes.FirstOrDefault(s => s.Guid.Value == loaded.Guid.Value);
        if (existing != null)
        {
            existing.Name = loaded.Name;
            existing.Entities.Clear();
            foreach (var entity in loaded.Entities) existing.Entities.Add(entity);
            worldEditor.ActiveScene = existing;
            return new { loaded = true, activated = true, scene = DescribeScene(existing, created: false) };
        }

        project.Scenes.Add(loaded);
        worldEditor.Scenes.Add(loaded);
        worldEditor.ActiveScene = loaded;
        worldEditor.MarkProjectDirty();
        return new { loaded = true, activated = true, scene = DescribeScene(loaded, created: false) };
    }

    private static string ResolveScenePath(Project project, string path)
    {
        if (Path.IsPathRooted(path)) return Path.GetFullPath(path);
        var contentRoot = AssetRegistry.ProjectContentRoot ?? project.Path;
        return Path.GetFullPath(Path.Combine(contentRoot, path));
    }

    private static object DescribeScene(Scene scene, bool created) => new
    {
        sceneGuid = scene.Guid.Value.ToString("D").ToLowerInvariant(),
        sceneName = scene.Name,
        isActive = ReferenceEquals(EditorServices.ActiveScene, scene),
        entityCount = scene.Entities.Count,
        created,
    };

    [CliCommand("asset.list",
        Description = "列出项目资产注册表中的资产（guid、类型、路径）。type 按资产类型名过滤" +
                      "（如 Mesh/Texture/Material，大小写不敏感）；filter 按路径子串过滤；" +
                      "limit 上限 500（默认 50）。返回 total 为过滤后总数，便于分页。",
        Example = """{"type":"Mesh","filter":"sponza","limit":20}""")]
    public object AssetList(string? type = null, string? filter = null, int limit = 50)
    {
        if (limit < 1) throw CliError.InvalidParams("limit 必须 ≥ 1");
        if (limit > 500) throw CliError.InvalidParams("limit 上限 500", "用 filter 缩小范围后再分页");

        RequireProject();
        var assets = AssetRegistry.Assets.AsEnumerable();
        if (!string.IsNullOrWhiteSpace(type))
        {
            var match = assets.Select(a => a.Type.ToString()).Distinct()
                .FirstOrDefault(t => t.Equals(type, StringComparison.OrdinalIgnoreCase));
            if (match == null)
                throw CliError.InvalidParams($"未知资产类型 '{type}'",
                    "有效取值: " + string.Join("/", assets.Select(a => a.Type.ToString()).Distinct().OrderBy(t => t)));
            assets = assets.Where(a => a.Type.ToString().Equals(match, StringComparison.OrdinalIgnoreCase));
        }
        if (!string.IsNullOrWhiteSpace(filter))
            assets = assets.Where(a => a.FullPath.Contains(filter, StringComparison.OrdinalIgnoreCase));

        var filtered = assets.ToArray();
        return new
        {
            total = filtered.Length,
            returned = Math.Min(filtered.Length, limit),
            assets = filtered.Take(limit).Select(a => new
            {
                guid = a.Guid.ToString("D"),
                type = a.Type.ToString(),
                path = a.FullPath,
            }).ToArray(),
        };
    }
}
