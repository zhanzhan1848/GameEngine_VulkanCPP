using System;
using System.Collections.Generic;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Prefabs;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// prefab 覆盖/传播 verbs（prefab S1，2026-10-15）。与纯逻辑层
/// PrefabOverrideService 共用一套语义；映射键=源实体名（实例化保留名）。
/// 覆盖范围 S1 = transform 九分量；根实体的 position 是布设意图，
/// capture/propagate 默认不触碰（includeRootPosition 显式开启）。
/// </summary>
public sealed class PrefabCommands
{
    private sealed record Context(
        WorldEditorViewModel WorldEditor, GameEntity Entity,
        PrefabInstanceComponent Component, PrefabDocument Source);

    private static Context Require(Guid entityGuid)
    {
        var worldEditor = EditorServices.WorldEditor
            ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");
        var entity = Sequence.SequenceDriver.FindEntity(worldEditor.ActiveScene?.Entities, entityGuid)
            ?? throw CliError.NotFound($"实体 {entityGuid}", "scene.tree 查看实体列表");
        var comp = entity.GetComponent<PrefabInstanceComponent>()
            ?? throw CliError.NotFound(
                $"实体 '{entity.Name}' 没有 PrefabInstance 组件", "只有 prefab 实例支持覆盖操作");
        var info = AssetRegistry.GetByGuid(comp.PrefabGuid.Value);
        var source = info == null ? null : PrefabSerializer.Read(info.FullPath);
        if (source == null)
            throw CliError.NotFound(
                $"源 prefab (guid={comp.PrefabGuid}) 不可读",
                "资产可能被移动/删除——用 asset.rescan 或重新导入后重试");
        return new Context(worldEditor, entity, comp, source);
    }

    private static List<GameEntity> SubtreeOf(GameEntity root)
    {
        var all = new List<GameEntity>();
        void Collect(GameEntity e)
        {
            all.Add(e);
            foreach (var c in e.Children) Collect(c);
        }
        Collect(root);
        return all;
    }

    [CliCommand("prefab.overrides",
        Description = "列出 prefab 实例的覆盖（prefab S1）。dryRun=true 额外执行一次捕获对比预览" +
                      "（实例 vs 源文档，不写状态）。返回已存条目 + 诊断（stale=覆盖指向源中已删实体、" +
                      "unmapped=实例实体在源中无同名、duplicateSources=源内重名）。纯读操作。",
        Example = """{"entityGuid":"…","dryRun":true}""")]
    public object Overrides(Guid entityGuid, bool dryRun = false)
    {
        var ctx = Require(entityGuid);
        var stored = PrefabOverrideService.Load(ctx.Component);
        object? preview = null;
        if (dryRun)
        {
            var (candidate, report) = PrefabOverrideService.Capture(ctx.Entity, SubtreeOf(ctx.Entity), ctx.Source);
            preview = new
            {
                pending = candidate.Select(o => new { o.EntityName, o.Property, o.Value }).ToArray(),
                staleOverrideNames = report.StaleOverrideNames.ToArray(),
                unmappedInstanceNames = report.UnmappedInstanceNames.ToArray(),
                duplicateSourceNames = report.DuplicateSourceNames.ToArray(),
            };
        }
        return new
        {
            entity = ctx.Entity.Name,
            prefabGuid = ctx.Component.PrefabGuid.ToString(),
            stored = stored.Select(o => new { o.EntityName, o.Property, o.Value }).ToArray(),
            preview,
        };
    }

    [CliCommand("prefab.capture",
        Description = "捕获 prefab 实例相对源文档的 transform 差异为覆盖（prefab S1）。" +
                      "值与源一致的条目会被移除（还原即清除）。根实体 position 默认不参与" +
                      "（布设意图；includeRootPosition=true 显式包含）。记入 undo。",
        Example = """{"entityGuid":"…"}""")]
    public object Capture(Guid entityGuid, bool includeRootPosition = false)
    {
        var ctx = Require(entityGuid);
        var (overrides, report) = PrefabOverrideService.Capture(
            ctx.Entity, SubtreeOf(ctx.Entity), ctx.Source, includeRootPosition);
        var undo = new PrefabOverridesUndoAction(ctx.Component);
        PrefabOverrideService.Store(ctx.Component, overrides);
        undo.Commit();
        ctx.WorldEditor.UndoRedo.Add(undo);
        return new
        {
            captured = overrides.Count,
            entries = overrides.Select(o => new { o.EntityName, o.Property, o.Value }).ToArray(),
            staleOverrideNames = report.StaleOverrideNames.ToArray(),
            unmappedInstanceNames = report.UnmappedInstanceNames.ToArray(),
        };
    }

    [CliCommand("prefab.revert",
        Description = "把覆盖的源值写回 prefab 实例并清除条目（prefab S1）。entityName 省略 = " +
                      "全部还原。源/实例已不存在的条目按清理处理并报告。记入 undo。",
        Example = """{"entityGuid":"…","entityName":"Capital"}""")]
    public object Revert(Guid entityGuid, string? entityName = null)
    {
        var ctx = Require(entityGuid);
        var undo = new PrefabOverridesUndoAction(ctx.Component);
        IReadOnlyCollection<string>? only = entityName == null ? null : new[] { entityName };
        var report = PrefabOverrideService.Revert(ctx.Entity, SubtreeOf(ctx.Entity), ctx.Source, ctx.Component, only);
        undo.Commit();
        ctx.WorldEditor.UndoRedo.Add(undo);
        ctx.WorldEditor.PushTransformsToNative(new[] { ctx.Entity });
        return new
        {
            reverted = report.Applied.Count,
            staleOverrideNames = report.StaleOverrideNames.ToArray(),
        };
    }

    [CliCommand("prefab.apply",
        Description = "把覆盖提升回源 prefab 文档并保存（prefab S1；实例覆盖值覆盖源值），" +
                      "随后清空实例覆盖。多个实例可逐个 apply——后写胜出（S1 无冲突面板，" +
                      "同属性多实例冲突的处理是 S2）。记入 undo（文档可整体还原）。",
        Example = """{"entityGuid":"…"}""")]
    public object Apply(Guid entityGuid)
    {
        var ctx = Require(entityGuid);
        var info = AssetRegistry.GetByGuid(ctx.Component.PrefabGuid.Value)
            ?? throw CliError.NotFound("源 prefab 资产不在注册表", "asset.rescan 后重试");
        var undo = new PrefabOverridesUndoAction(ctx.Component);
        var (modified, report) = PrefabOverrideService.Apply(ctx.Source, ctx.Component);
        undo.Commit();
        ctx.WorldEditor.UndoRedo.Add(undo);
        PrefabSerializer.Write(info.FullPath, ctx.Source);
        return new
        {
            applied = true,
            modifiedEntities = modified.ToArray(),
            prefabPath = info.FullPath,
            staleOverrideNames = report.StaleOverrideNames.ToArray(),
        };
    }

    [CliCommand("prefab.propagate",
        Description = "源 prefab 更新后刷新实例（prefab S1）：无覆盖的属性取源新值，" +
                      "有覆盖的属性保留实例值（合并语义）；根实体 position 不触碰。" +
                      "实例侧新增的源实体不在此路（重新实例化）——S1 语义如实声明。记入 undo。",
        Example = """{"entityGuid":"…"}""")]
    public object Propagate(Guid entityGuid)
    {
        var ctx = Require(entityGuid);
        var report = PrefabOverrideService.Propagate(
            ctx.Entity, SubtreeOf(ctx.Entity), ctx.Source, ctx.Component);
        var touched = report.Applied.Select(a => a.EntityName).Distinct().ToList();
        var entities = SubtreeOf(ctx.Entity)
            .Where(e => touched.Contains(e.Name))
            .ToList();
        if (entities.Count > 0)
            ctx.WorldEditor.PushTransformsToNative(entities);
        return new
        {
            updatedProperties = report.Applied.Count,
            updatedEntities = touched.ToArray(),
            unmappedInstanceNames = report.UnmappedInstanceNames.ToArray(),
        };
    }
}
