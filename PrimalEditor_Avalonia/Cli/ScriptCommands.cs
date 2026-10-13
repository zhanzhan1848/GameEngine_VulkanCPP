using System;
using System.Collections.Generic;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// scripting S1 CLI 面（与 Inspector/拖放同一文档模型与同步通道）。
/// 单位契约：参数值为字符串（引擎侧 Lua params 表，Lua 算术自然转型）；
/// 实体旋转语义为弧度（引擎 SetEntityTransform 全链约定）。
/// </summary>
public sealed class ScriptCommands
{
    private static WorldEditorViewModel RequireWorldEditor() =>
        EditorServices.WorldEditor
        ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");

    private static GameEntity RequireEntity(Guid entityGuid)
    {
        if (entityGuid == Guid.Empty)
            throw CliError.InvalidParams("entityGuid 不能为空 Guid", "先用 scene.tree 查询有效 guid");
        var scene = RequireWorldEditor().ActiveScene
            ?? throw CliError.NotReady("当前没有激活场景", "先在编辑器中打开项目");
        return FindEntity(scene.Entities, entityGuid)
            ?? throw CliError.NotFound($"实体 {entityGuid.ToString().ToLowerInvariant()}",
                "先用 scene.tree 查询有效 guid");
    }

    private static GameEntity? FindEntity(IEnumerable<GameEntity> entities, Guid guid)
    {
        foreach (var entity in entities)
        {
            if (entity.EntityGuid.Value == guid) return entity;
            var child = FindEntity(entity.Children, guid);
            if (child != null) return child;
        }
        return null;
    }

    // === 挂载 / 换绑 ===

    [CliCommand("script.attach",
        Description = "把 Content 内的 .lua 脚本资产挂到实体（已有 LuaScriptComponent 则换绑）。" +
                      "前置：编辑器已打开项目、脚本资产在 Content 内且已注册。parametersJson 为 JSON 对象字符串" +
                      "（键不得含空格；值为字符串）。失败：实体/资产不存在抛 NOT_FOUND，参数非法抛 INVALID_PARAMS。改动走 UndoRedo。",
        Example = """{"entityGuid":"…","path":"Content/Scripts/rotate.lua","parametersJson":"{\"speed\":\"0.8\"}"}""")]
    public object ScriptAttach(
        Guid entityGuid,
        string path,
        string? parametersJson = null)
    {
        var entity = RequireEntity(entityGuid);
        var worldEditor = RequireWorldEditor();

        var info = AssetRegistry.Get(System.IO.Path.GetFullPath(path))
            ?? throw CliError.NotFound($"脚本资产 '{path}'",
                "资产必须在项目 Content 内且已被注册表扫描（Content Browser 可见）");
        if (!info.FullPath.EndsWith(".lua", StringComparison.OrdinalIgnoreCase))
            throw CliError.InvalidParams("仅支持 .lua 脚本（.py/.cs 无生产后端）");
        Dictionary<string, string>? parameters = null;
        if (!string.IsNullOrWhiteSpace(parametersJson))
        {
            try
            {
                parameters = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, string>>(parametersJson);
            }
            catch (System.Text.Json.JsonException ex)
            {
                throw CliError.InvalidParams($"parametersJson 不是合法的 JSON 对象：{ex.Message}",
                    """示例: "{\"speed\":\"0.8\"}" """.Trim());
            }
        }
        if (parameters is { Count: > 0 } && parameters.Keys.Any(k => string.IsNullOrWhiteSpace(k) || k.Contains(' ')))
            throw CliError.InvalidParams("param 键不得为空或含空格（运行时包行式解析约束）");

        var script = entity.GetComponent<LuaScriptComponent>();
        var hadScript = script != null;
        var oldGuid = script?.ScriptAssetGuid ?? default;
        var oldName = script?.ScriptName ?? "";
        var oldParams = script?.Parameters is { } p
            ? new Dictionary<string, string>(p) : new Dictionary<string, string>();
        script ??= new LuaScriptComponent();
        if (!hadScript) entity.Components.Add(script);

        script.ScriptAssetGuid = new AssetGuid(info.Guid);
        script.ScriptName = info.FileName;
        script.Parameters = parameters is { Count: > 0 }
            ? new Dictionary<string, string>(parameters) : new Dictionary<string, string>();

        var newGuid = script.ScriptAssetGuid;
        var newName = script.ScriptName;
        var newParams = new Dictionary<string, string>(script.Parameters);
        worldEditor.UndoRedo.Add(new DelegateAction("script.attach",
            () =>
            {
                if (!hadScript) { entity.Components.Remove(script); }
                else { script.ScriptAssetGuid = oldGuid; script.ScriptName = oldName; script.Parameters = new Dictionary<string, string>(oldParams); }
                if (NativeEngineBridge.IsLoaded) worldEditor.SceneSynchronizer.SyncEntity(entity);
            },
            () =>
            {
                if (!hadScript && !entity.Components.Contains(script)) entity.Components.Add(script);
                script.ScriptAssetGuid = newGuid; script.ScriptName = newName;
                script.Parameters = new Dictionary<string, string>(newParams);
                if (NativeEngineBridge.IsLoaded) worldEditor.SceneSynchronizer.SyncEntity(entity);
            }));

        if (NativeEngineBridge.IsLoaded) worldEditor.SceneSynchronizer.SyncEntity(entity);
        worldEditor.MarkProjectDirty();
        return new
        {
            entity = entity.Name,
            script = info.FileName,
            assetGuid = info.Guid.ToString("D").ToLowerInvariant(),
            paramCount = script.Parameters.Count,
        };
    }

    // === 参数热改 ===

    [CliCommand("script.set-param",
        Description = "设置/更新/删除实体脚本的单个参数（value=null 删除该键）。改动立即销毁重建引擎实例" +
                      "（begin_play 以新参数重放），走 UndoRedo。失败：无 LuaScriptComponent 或键含空格。",
        Example = """{"entityGuid":"…","key":"speed","value":"1.5"}""")]
    public object ScriptSetParam(
 Guid entityGuid,
 string key,
 string? value)
    {
        if (string.IsNullOrWhiteSpace(key) || key.Contains(' '))
            throw CliError.InvalidParams("key 不得为空或含空格");
        var entity = RequireEntity(entityGuid);
        var worldEditor = RequireWorldEditor();
        var script = entity.GetComponent<LuaScriptComponent>()
            ?? throw CliError.NotFound($"实体 '{entity.Name}' 没有 LuaScriptComponent",
                "script.attach 先挂载");

        var oldParams = new Dictionary<string, string>(script.Parameters);
        if (value is null) script.Parameters.Remove(key);
        else script.Parameters[key] = value;
        var newParams = new Dictionary<string, string>(script.Parameters);

        worldEditor.UndoRedo.Add(new DelegateAction($"script.set-param {key}",
            () => { script.Parameters = new Dictionary<string, string>(oldParams); if (NativeEngineBridge.IsLoaded) worldEditor.SceneSynchronizer.SyncEntity(entity); },
            () => { script.Parameters = new Dictionary<string, string>(newParams); if (NativeEngineBridge.IsLoaded) worldEditor.SceneSynchronizer.SyncEntity(entity); }));
        if (NativeEngineBridge.IsLoaded) worldEditor.SceneSynchronizer.SyncEntity(entity);
        worldEditor.MarkProjectDirty();
        return new
        {
            key,
            value = value is null ? null : value,
            removed = value is null,
        };
    }

    // === 查询 ===

    [CliCommand("script.info",
        Description = "查询实体脚本组件状态（资产 GUID/名字/路径/参数表）。只读，无副作用。" +
                      "失败：实体不存在或无 LuaScriptComponent。",
        Example = """{"entityGuid":"…"}""")]
    public object ScriptInfo(
 Guid entityGuid)
    {
        var entity = RequireEntity(entityGuid);
        var script = entity.GetComponent<LuaScriptComponent>()
            ?? throw CliError.NotFound($"实体 '{entity.Name}' 没有 LuaScriptComponent", "script.attach 先挂载");
        var info = AssetRegistry.GetByGuid(script.ScriptAssetGuid.Value);
        return new
        {
            entity = entity.Name,
            assetGuid = script.ScriptAssetGuid.ToString(),
            name = script.ScriptName,
            path = info?.FullPath ?? "(unregistered)",
            parameters = script.Parameters.ToDictionary(kv => kv.Key, kv => kv.Value),
        };
    }
}
