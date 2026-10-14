using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject.Documents;

namespace PrimalEditor_Avalonia.Content.Prefabs;

/// <summary>单条覆盖（prefab S1）：transform 九分量之一。
/// 键 = 源文档实体名（实例化保留名字、guid 每次换新——名字即稳定映射）。</summary>
public sealed class PrefabOverride
{
    [System.Text.Json.Serialization.JsonPropertyName("entityName")]
    public string EntityName { get; set; } = "";

    /// <summary>S1 固定 "transform"；组件级覆盖是后续阶段。</summary>
    [System.Text.Json.Serialization.JsonPropertyName("component")]
    public string Component { get; set; } = "transform";

    /// <summary>posX/posY/posZ/rotX/rotY/rotZ/sclX/sclY/sclZ。</summary>
    [System.Text.Json.Serialization.JsonPropertyName("property")]
    public string Property { get; set; } = "";

    [System.Text.Json.Serialization.JsonPropertyName("value")]
    public float Value { get; set; }
}

/// <summary>各操作的结构化报告（诊断不静默：unmapped/stale 全列出）。</summary>
public sealed class PrefabOverrideReport
{
    public List<PrefabOverride> Applied { get; } = new();
    public List<string> StaleOverrideNames { get; } = new();   // 覆盖指向源中不存在的实体名
    public List<string> UnmappedInstanceNames { get; } = new(); // 实例实体在源中无同名实体
    public List<string> DuplicateSourceNames { get; } = new();  // 源文档内重名（映射取首个）
    public bool SourceMissing { get; set; }                     // 源 prefab 不可读（调用方置位）
}

/// <summary>
/// Prefab 覆盖/传播服务（prefab S1，2026-10-15）。纯逻辑层——编辑器命令、
/// CLI 与测试共用；不持有状态，全部操作显式传入文档与实例子树。
///
/// S1 语义（阶段合同）：
///   - 映射键 = 源实体名（InstantiatePrefabAsync 保留名、换 guid）；
///   - 覆盖范围 = transform 九分量（组件级值覆盖是 S2+）；
///   - 根实体 position 是布设意图：捕获/传播默认跳过（includeRootPosition
///     显式开启），旋转/缩放照常参与；
///   - 未映射的实例实体 / 指向已删源实体的覆盖都报告、不静默丢弃；
///   - 操作幂等：值回到源值时捕获会移除对应覆盖（"还原即清除"）。
/// </summary>
public static class PrefabOverrideService
{
    private static readonly string[] PropertyOrder =
        { "posX", "posY", "posZ", "rotX", "rotY", "rotZ", "sclX", "sclY", "sclZ" };

    // ---------------------------------------------------------------- 模型序列化

    /// <summary>Overrides 字典 ⇄ 结构化覆盖列表（每实体名一条 JSON 数组）。
    /// 不可解析的旧 blob 保留原样并经 rawOut 报告（迁移期不静默丢数据）。</summary>
    public static List<PrefabOverride> Load(PrefabInstanceComponent comp, List<string>? rawOut = null)
    {
        var result = new List<PrefabOverride>();
        foreach (var (name, json) in comp.Overrides)
        {
            try
            {
                var list = JsonSerializer.Deserialize<List<PrefabOverride>>(json);
                if (list == null) { rawOut?.Add(name); continue; }
                foreach (var o in list)
                {
                    o.EntityName = name;
                    result.Add(o);
                }
            }
            catch (JsonException)
            {
                rawOut?.Add(name);
            }
        }
        return result;
    }

    public static void Store(PrefabInstanceComponent comp, IEnumerable<PrefabOverride> overrides)
    {
        comp.Overrides.Clear();
        foreach (var group in overrides.GroupBy(o => o.EntityName).OrderBy(g => g.Key, StringComparer.Ordinal))
            comp.Overrides[group.Key] = JsonSerializer.Serialize(group.ToList());
    }

    // ---------------------------------------------------------------- 捕获（实例 → 覆盖）

    /// <summary>比较实例子树与源文档的 transform，产出完整覆盖集（不写组件——
    /// 调用方决定落盘与 undo）。值一致时该条目不出现（还原即清除语义）。</summary>
    public static (List<PrefabOverride> overrides, PrefabOverrideReport report) Capture(
        GameEntity root, IReadOnlyCollection<GameEntity> subtree,
        PrefabDocument source, bool includeRootPosition = false)
    {
        var report = new PrefabOverrideReport();
        var (map, duplicates) = MapSourceEntities(source);
        report.DuplicateSourceNames.AddRange(duplicates);

        var result = new List<PrefabOverride>();
        foreach (var entity in subtree)
        {
            var transform = entity.GetTransform();
            if (transform == null) continue;
            if (!map.TryGetValue(entity.Name, out var doc))
            {
                report.UnmappedInstanceNames.Add(entity.Name);
                continue;
            }
            var src = ReadSourceTransform(doc);
            if (src == null) continue;
            var isRoot = ReferenceEquals(entity, root);
            foreach (var prop in PropertyOrder)
            {
                if (isRoot && !includeRootPosition && prop.StartsWith("pos")) continue;
                var instanceValue = ReadProperty(transform, prop);
                var sourceValue = src.GetValueOrDefault(prop);
                if (Math.Abs(instanceValue - sourceValue) > 1e-6f)
                    result.Add(new PrefabOverride
                    {
                        EntityName = entity.Name,
                        Property = prop,
                        Value = instanceValue,
                    });
            }
        }
        ReportStale(result, map, report);
        return (result, report);
    }

    // ---------------------------------------------------------------- Revert（覆盖 → 源值写回实例）

    /// <summary>把已存覆盖的源值写回实例（全部或指定实体名），并移除这些条目。</summary>
    public static PrefabOverrideReport Revert(
        GameEntity root, IReadOnlyCollection<GameEntity> subtree,
        PrefabDocument source, PrefabInstanceComponent comp,
        IReadOnlyCollection<string>? onlyEntityNames = null)
    {
        var report = new PrefabOverrideReport();
        var (map, duplicates) = MapSourceEntities(source);
        report.DuplicateSourceNames.AddRange(duplicates);
        var entities = subtree.ToDictionary(e => e.Name, e => e, StringComparer.Ordinal);
        var remaining = new List<PrefabOverride>();

        foreach (var o in Load(comp, rawOut: null))
        {
            if (onlyEntityNames != null && !onlyEntityNames.Contains(o.EntityName)) { remaining.Add(o); continue; }
            if (!map.TryGetValue(o.EntityName, out var doc)
                || !entities.TryGetValue(o.EntityName, out var entity)
                || entity.GetTransform() is not { } transform
                || ReadSourceTransform(doc) is not { } src)
            {
                if (!map.ContainsKey(o.EntityName)) report.StaleOverrideNames.Add(o.EntityName);
                continue;   // 源/实例缺失：不写值、不保留（实体已删的覆盖随 revert 清理）
            }
            WriteProperty(transform, o.Property, src.GetValueOrDefault(o.Property));
            report.Applied.Add(o);
        }
        Store(comp, remaining);
        return report;
    }

    // ---------------------------------------------------------------- Apply（覆盖提升回源文档）

    /// <summary>把覆盖值写进源文档（调用方负责 PrefabSerializer.Write 持久化）
    /// 并清空覆盖。返回被修改的源实体名集合。</summary>
    public static (List<string> modifiedEntities, PrefabOverrideReport report) Apply(
        PrefabDocument source, PrefabInstanceComponent comp)
    {
        var report = new PrefabOverrideReport();
        var (map, duplicates) = MapSourceEntities(source);
        report.DuplicateSourceNames.AddRange(duplicates);
        var modified = new List<string>();
        var byName = Load(comp).GroupBy(o => o.EntityName);

        foreach (var group in byName)
        {
            if (!map.TryGetValue(group.Key, out var doc))
            {
                report.StaleOverrideNames.Add(group.Key);
                continue;
            }
            var src = ReadSourceTransformForWrite(doc) ?? CreateTransformDocument(doc);
            foreach (var o in group)
            {
                SetDocumentValue(src, o.Property, o.Value);
            }
            if (!modified.Contains(group.Key)) modified.Add(group.Key);
        }
        comp.Overrides.Clear();
        return (modified, report);
    }

    // ---------------------------------------------------------------- 源更新传播

    /// <summary>源文档变化后刷新实例：无覆盖的属性取源值；有覆盖的属性保留
    /// 实例覆盖值（合并语义）。根 position 仍不触碰（布设意图）。</summary>
    public static PrefabOverrideReport Propagate(
        GameEntity root, IReadOnlyCollection<GameEntity> subtree,
        PrefabDocument source, PrefabInstanceComponent comp)
    {
        var report = new PrefabOverrideReport();
        var (map, duplicates) = MapSourceEntities(source);
        report.DuplicateSourceNames.AddRange(duplicates);
        var overridden = Load(comp)
            .GroupBy(o => o.EntityName)
            .ToDictionary(g => g.Key, g => g.Select(o => o.Property).ToHashSet(), StringComparer.Ordinal);

        foreach (var entity in subtree)
        {
            if (entity.GetTransform() is not { } transform) continue;
            if (!map.TryGetValue(entity.Name, out var doc)) { report.UnmappedInstanceNames.Add(entity.Name); continue; }
            var src = ReadSourceTransform(doc);
            if (src == null) continue;
            var isRoot = ReferenceEquals(entity, root);
            overridden.TryGetValue(entity.Name, out var props);
            foreach (var prop in PropertyOrder)
            {
                if (isRoot && prop.StartsWith("pos")) continue;
                if (props != null && props.Contains(prop)) continue;   // 覆盖保留
                WriteProperty(transform, prop, src.GetValueOrDefault(prop));
                report.Applied.Add(new PrefabOverride { EntityName = entity.Name, Property = prop });
            }
        }
        return report;
    }

    // ---------------------------------------------------------------- 内部

    private static (Dictionary<string, EntityDocument> map, List<string> duplicates) MapSourceEntities(
        PrefabDocument source)
    {
        var map = new Dictionary<string, EntityDocument>(StringComparer.Ordinal);
        var duplicates = new List<string>();
        foreach (var e in source.Entities)
        {
            if (!map.TryAdd(e.Name, e)) duplicates.Add(e.Name);
        }
        return (map, duplicates);
    }

    private static void ReportStale(List<PrefabOverride> overrides,
        Dictionary<string, EntityDocument> map, PrefabOverrideReport report)
    {
        foreach (var name in overrides.Select(o => o.EntityName).Distinct())
            if (!map.ContainsKey(name)) report.StaleOverrideNames.Add(name);
    }

    private static Dictionary<string, float>? ReadSourceTransform(EntityDocument doc)
    {
        var comp = doc.Components.FirstOrDefault(c =>
            c.Type.Equals("transform", StringComparison.OrdinalIgnoreCase) ||
            c.Type.Equals("Transform", StringComparison.Ordinal));
        if (comp == null) return null;
        try
        {
            var t = JsonSerializer.Deserialize<PrimalEditor_Avalonia.Components.Transform>(
                comp.Data.GetRawText(), SceneJson.Options);
            if (t == null) return null;
            return new Dictionary<string, float>
            {
                ["posX"] = t.PositionX, ["posY"] = t.PositionY, ["posZ"] = t.PositionZ,
                ["rotX"] = t.RotationX, ["rotY"] = t.RotationY, ["rotZ"] = t.RotationZ,
                ["sclX"] = t.ScaleX, ["sclY"] = t.ScaleY, ["sclZ"] = t.ScaleZ,
            };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    /// <summary>取源 transform 组件的可写引用（无则 null——Apply 走建新路径）。</summary>
    private static ComponentDocument? ReadSourceTransformForWrite(EntityDocument doc) =>
        doc.Components.FirstOrDefault(c => c.Type.Equals("Transform", StringComparison.Ordinal));

    private static ComponentDocument CreateTransformDocument(EntityDocument doc)
    {
        var comp = new ComponentDocument { Type = "Transform", Data = JsonSerializer.SerializeToElement(
            new PrimalEditor_Avalonia.Components.Transform(), SceneJson.Options) };
        doc.Components.Add(comp);
        return comp;
    }

    private static void SetDocumentValue(ComponentDocument transformDoc, string property, float value)
    {
        var t = JsonSerializer.Deserialize<PrimalEditor_Avalonia.Components.Transform>(
            transformDoc.Data.GetRawText(), SceneJson.Options) ?? new PrimalEditor_Avalonia.Components.Transform();
        switch (property)
        {
            case "posX": t.PositionX = value; break;
            case "posY": t.PositionY = value; break;
            case "posZ": t.PositionZ = value; break;
            case "rotX": t.RotationX = value; break;
            case "rotY": t.RotationY = value; break;
            case "rotZ": t.RotationZ = value; break;
            case "sclX": t.ScaleX = value; break;
            case "sclY": t.ScaleY = value; break;
            case "sclZ": t.ScaleZ = value; break;
        }
        transformDoc.Data = JsonSerializer.SerializeToElement(t, SceneJson.Options);
    }

    private static float ReadProperty(PrimalEditor_Avalonia.Components.Transform t, string property) =>
        property switch
        {
            "posX" => t.PositionX, "posY" => t.PositionY, "posZ" => t.PositionZ,
            "rotX" => t.RotationX, "rotY" => t.RotationY, "rotZ" => t.RotationZ,
            "sclX" => t.ScaleX, "sclY" => t.ScaleY, "sclZ" => t.ScaleZ,
            _ => 0f,
        };

    private static void WriteProperty(PrimalEditor_Avalonia.Components.Transform t, string property, float value)
    {
        switch (property)
        {
            case "posX": t.PositionX = value; break;
            case "posY": t.PositionY = value; break;
            case "posZ": t.PositionZ = value; break;
            case "rotX": t.RotationX = value; break;
            case "rotY": t.RotationY = value; break;
            case "rotZ": t.RotationZ = value; break;
            case "sclX": t.ScaleX = value; break;
            case "sclY": t.ScaleY = value; break;
            case "sclZ": t.ScaleZ = value; break;
        }
    }
}

/// <summary>场景序列化选项（与 Serializer 的 Transform 文档同口径的窄入口）。</summary>
internal static class SceneJson
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNameCaseInsensitive = true,
    };
}

/// <summary>覆盖字典整体替换的 undo/redo（prefab S1）——快照 JSON 整体还原，
/// 不做逐条 diff（字典规模 = 实体数级别，整体快照足够小且绝对正确）。</summary>
public sealed class PrefabOverridesUndoAction : PrimalEditor_Avalonia.Common.IUndoRedoAction
{
    private readonly PrefabInstanceComponent _component;
    private readonly string _before;
    private string _after = "";

    public string Name => "Prefab Overrides";

    public PrefabOverridesUndoAction(PrefabInstanceComponent component)
    {
        _component = component;
        _before = Serialize(_component);
    }

    /// <summary>变更完成后调用（捕获 after 快照）。</summary>
    public void Commit() => _after = Serialize(_component);

    public void Undo() => Restore(_before);
    public void Redo() => Restore(_after);

    private static string Serialize(PrefabInstanceComponent c) =>
        JsonSerializer.Serialize(c.Overrides,
            new JsonSerializerOptions { WriteIndented = false });

    private void Restore(string json)
    {
        var dict = JsonSerializer.Deserialize<Dictionary<string, string>>(json) ?? new();
        _component.Overrides.Clear();
        foreach (var kv in dict) _component.Overrides[kv.Key] = kv.Value;
    }
}
