using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Collab;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.CollabNs;

/// <summary>
/// editor-collaboration S1 契约测试：跨资产事务 all-or-nothing（前像冲突→零写入+
/// 冲突清单）、逆应用（事务 Undo）、同文档重复变更拒绝、语义差异（增/删/改/
/// 嵌套路径/数组整体/数值相等/排序确定）。
/// </summary>
public sealed class CollabTests
{
    private sealed class Store : Dictionary<string, string> { }

    private static DocumentChange Change(string id, string before, string after) =>
        new() { DocId = id, BeforeJson = before, AfterJson = after };

    [Fact]
    public void Transaction_AllOrNothing_NoPartialWriteOnConflict()
    {
        var store = new Store
        {
            ["scene"] = "v1",
            ["material"] = "m1",
        };
        var result = CollabTransaction.Apply(new[]
        {
            Change("scene", "v1", "v2"),
            Change("material", "m1-stale", "m2"),   // 前像不匹配（已被他方改）
        }, id => store.TryGetValue(id, out var v) ? v : null,
           (id, v) => store[id] = v);

        Assert.False(result.Applied);
        Assert.Equal(new[] { "material" }, result.ConflictedDocIds);
        Assert.Equal("v1", store["scene"]);        // 零写入（无半事务）
        Assert.Equal("m1", store["material"]);

        // 冲突修复后重试成功
        var retry = CollabTransaction.Apply(new[]
        {
            Change("scene", "v1", "v2"),
            Change("material", "m1", "m2"),
        }, id => store.TryGetValue(id, out var v) ? v : null,
           (id, v) => store[id] = v);
        Assert.True(retry.Applied);
        Assert.Equal("v2", store["scene"]);
    }

    [Fact]
    public void Transaction_MissingDoc_IsConflict_NotSilentCreate()
    {
        var store = new Store { ["a"] = "1" };
        var result = CollabTransaction.Apply(new[] { Change("ghost", "x", "y") },
            id => store.TryGetValue(id, out var v) ? v : null,
            (id, v) => store[id] = v);
        Assert.False(result.Applied);
        Assert.Equal(new[] { "ghost" }, result.ConflictedDocIds);
        Assert.False(store.ContainsKey("ghost"));   // 不经事务静默创建
    }

    [Fact]
    public void Transaction_Revert_UndoesAsAtomicUnit()
    {
        var store = new Store { ["a"] = "1", ["b"] = "1" };
        var changes = new[] { Change("a", "1", "2"), Change("b", "1", "2") };
        var apply = CollabTransaction.Apply(changes,
            id => store[id], (id, v) => store[id] = v);
        Assert.True(apply.Applied);

        var revert = CollabTransaction.Revert(changes,
            id => store[id], (id, v) => store[id] = v);
        Assert.True(revert.Applied);
        Assert.Equal("1", store["a"]);
        Assert.Equal("1", store["b"]);
    }

    [Fact]
    public void Transaction_DuplicateDocInOneTransaction_Rejected()
    {
        Assert.Throws<InvalidOperationException>(() => CollabTransaction.Apply(
            new[] { Change("a", "1", "2"), Change("a", "2", "3") },
            _ => null, (_, _) => { }));
    }

    [Fact]
    public void SemanticDiff_AddRemoveChangeAndNesting()
    {
        var before = """
            { "name": "entity", "mass": 12.5, "tags": ["a","b"],
              "transform": { "x": 1, "y": 2 } }
            """;
        var after = """
            { "name": "entity2", "mass": 12.50, "tags": ["a"],
              "transform": { "x": 1, "z": 9 } }
            """;
        var diff = SemanticDiff.Between(before, after);
        var byPath = diff.ToDictionary(d => d.Path, d => d.Kind);

        Assert.Equal("change", byPath["name"]);
        Assert.Equal("remove", byPath["transform.y"]);
        Assert.Equal("add", byPath["transform.z"]);
        Assert.Equal("change", byPath["tags"]);        // 数组按值整体
        Assert.False(byPath.ContainsKey("mass"));      // 12.5 == 12.50（数值相等）
        Assert.False(byPath.ContainsKey("transform.x"));
        // Ordinal 排序确定性
        Assert.Equal(diff.OrderBy(d => d.Path, System.StringComparer.Ordinal).Select(d => d.Path).ToArray(),
            diff.Select(d => d.Path).ToArray());
    }

    [Fact]
    public void SemanticDiff_IdenticalDocs_Empty()
    {
        var json = """{ "a": 1, "list": [1,2,3], "nested": { "b": "x" } }""";
        Assert.Empty(SemanticDiff.Between(json, json));
    }
}
