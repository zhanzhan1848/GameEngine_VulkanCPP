using System;
using System.Linq;
using PrimalEditor_Avalonia.Industrial;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.IndustrialNs;

/// <summary>
/// industrial-models S1 契约测试：装配树/属性元数据、序列化往返+未知版本拒、
/// 校验（空/重复 PartId、重复源引用）、源更新语义（SourceRef 命中→身份稳定、
/// 源属性覆盖/User 属性保留、源缺失→Stale 保留不静默删、新增→确定性新 ID）。
/// </summary>
public sealed class AssemblyModelTests
{
    private static AssemblyPart Part(string id, string name, string file, string path,
        params (string key, string value, PropertyOrigin origin)[] props) => new()
    {
        PartId = id, Name = name, SourceFile = file, SourcePath = path,
        Properties = props.Select(p => new PartProperty
        {
            Key = p.key, Value = p.value, Origin = p.origin,
        }).ToList(),
    };

    private static AssemblyDocument Existing() => new()
    {
        Name = "pump",
        Roots =
        {
            Part("prt-root", "Pump", "pump.step", "",
                ("mass", "12.5", PropertyOrigin.Source),
                ("maintainer", "zhang", PropertyOrigin.User)),
            new AssemblyPart
            {
                PartId = "prt-body", Name = "Body", SourceFile = "pump.step", SourcePath = "body",
                Children = { Part("prt-flange", "Flange", "pump.step", "flange",
                    ("material", "steel", PropertyOrigin.Source)) },
            },
        },
    };

    [Fact]
    public void Document_RoundTrip_And_FutureSchemaRejected()
    {
        var doc = Existing();
        var (loaded, err) = AssemblyDocument.Load(doc.Serialize());
        Assert.Null(err);
        Assert.NotNull(loaded);
        Assert.Equal(3, loaded!.AllParts().Count());
        Assert.Equal("zhang", loaded.AllParts().First(p => p.PartId == "prt-root")
            .Properties.Single(x => x.Key == "maintainer").Value);

        var (future, futureErr) = AssemblyDocument.Load(
            doc.Serialize().Replace("\"schemaVersion\": 1", "\"schemaVersion\": 99"));
        Assert.Null(future);
        Assert.Contains("不静默覆写", futureErr);
    }

    [Fact]
    public void Validation_DuplicateIdsAndSourceRefs_Reported()
    {
        var doc = new AssemblyDocument
        {
            Roots =
            {
                Part("dup", "A", "a.step", "x"),
                Part("dup", "B", "a.step", "x"),        // 重复 ID + 重复源引用
                Part("", "NoId", "b.step", "y"),        // 空 ID
            },
        };
        var errors = doc.Validate();
        Assert.Contains(errors, e => e.Contains("重复 PartId: dup"));
        Assert.Contains(errors, e => e.Contains("重复源引用: a.step#x"));
        Assert.Contains(errors, e => e.Contains("缺 PartId"));
        Assert.Equal(3, errors.Count);
    }

    [Fact]
    public void Merge_SourceRefMatch_KeepsIdentity_UpdatesSourceProps_PreservesUser()
    {
        var existing = Existing();
        // 源重提取：body material 变了、root mass 变了、maintainer 不在源里（用户属性）
        var fresh = new AssemblyDocument
        {
            Roots =
            {
                Part("ignored-1", "Pump", "pump.step", "",
                    ("mass", "13.0", PropertyOrigin.Source)),
                Part("ignored-2", "Body", "pump.step", "body"),
            },
        };
        var (updated, report) = AssemblyUpdateService.Merge(existing, fresh, "prt");

        var root = updated.AllParts().Single(p => p.PartId == "prt-root");
        Assert.Equal("13.0", root.Properties.Single(p => p.Key == "mass").Value);       // 源属性覆盖
        Assert.Equal("zhang", root.Properties.Single(p => p.Key == "maintainer").Value); // 用户属性保留
        Assert.Equal(PropertyOrigin.User, root.Properties.Single(p => p.Key == "maintainer").Origin);

        var body = updated.AllParts().Single(p => p.PartId == "prt-body");               // 身份稳定
        Assert.Contains("prt-body", report.MatchedBySourceRef);
        Assert.Contains("prt-root:maintainer", report.UserPropertiesPreserved);
        // 原 body 的 flange 子部件（fresh 源没有引用它）→ Stale
        Assert.Contains("prt-flange", report.StalePartIds);
        Assert.Contains(report.MissingParts, m => m.Contains("prt-flange"));
    }

    [Fact]
    public void Merge_NewSourcePart_GetsDeterministicNewId()
    {
        var existing = Existing();
        var fresh = new AssemblyDocument
        {
            Roots =
            {
                Part("s-1", "Pump", "pump.step", "",
                    ("mass", "13.0", PropertyOrigin.Source)),
                Part("s-2", "Body", "pump.step", "body"),
                Part("s-3", "Motor", "motor.step", "m1",                     // 源新增
                    ("power", "3kW", PropertyOrigin.Source)),
            },
        };
        var (updated, report) = AssemblyUpdateService.Merge(existing, fresh, "prt");
        var motor = updated.AllParts().Single(p => p.Name == "Motor");
        Assert.StartsWith("prt-", motor.PartId);
        Assert.Equal("3kW", motor.Properties.Single(p => p.Key == "power").Value);
        Assert.Single(report.AddedParts);

        // 同一 fresh 源再合并一次 → 新 ID 一致（确定性）；已存在则不再新增
        var (again, report2) = AssemblyUpdateService.Merge(updated, fresh, "prt");
        Assert.Empty(report2.AddedParts);                                     // 第二轮全命中
        Assert.Equal(motor.PartId, again.AllParts().Single(p => p.Name == "Motor").PartId);
    }
}
