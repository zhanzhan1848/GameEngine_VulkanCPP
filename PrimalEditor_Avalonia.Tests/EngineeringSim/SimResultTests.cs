using System;
using System.Linq;
using PrimalEditor_Avalonia.EngineeringSim;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.EngineeringSimNs;

/// <summary>
/// engineering-simulation S1 契约测试：CSV 参考算例（结构化错误逐行报告、
/// 结构性错误整体拒绝）、数据集序列化往返 + 未知 schemaVersion 拒绝、
/// 单位一致性检查（不静默）、跨单位读取（kPa→bar 仿射复用）。
/// </summary>
public sealed class SimResultTests
{
    private const string ReferenceCsv =
        "# solver export v1.2\n" +
        "node,field,value,unit\n" +
        "beam-01,stress,2.5e6,Pa\n" +
        "beam-02,stress,3100000,Pa\n" +
        "beam-01,displacement,0.0012,m\n";

    [Fact]
    public void Csv_ReferenceCase_ParsesWithMetadata()
    {
        var (dataset, errors) = SolverCsvAdapter.Parse(ReferenceCsv, "linear-fea", "1.2", "run-42");
        Assert.NotNull(dataset);
        Assert.Empty(errors);
        Assert.Equal("linear-fea", dataset!.SolverName);
        Assert.Equal("run-42", dataset.RunId);
        Assert.Equal(2, dataset.Fields.Count);   // 按节点计：beam-01/beam-02
        Assert.Equal(2.5e6, dataset.Fields["beam-01"]["stress"].Value, 6);
        Assert.Equal("Pa", dataset.Fields["beam-02"]["stress"].Unit);
        Assert.Equal(0.0012, dataset.Fields["beam-01"]["displacement"].Value, 9);
        Assert.Equal(2, dataset.Fields["beam-01"].Count);   // beam-01 两个字段
    }

    [Fact]
    public void Csv_MalformedRows_AreReportedPerLine_NotFatal()
    {
        var csv =
            "node,field,value,unit\n" +
            "ok,temp,25,degC\n" +
            ",temp,30,degC\n" +          // 空 node
            "bad,temp,NaN,degC\n" +      // 非有限数值
            "bad,temp,not-a-number\n" +  // 非数值 + 列数不足
            "ok,temp,26,degC\n";
        var (dataset, errors) = SolverCsvAdapter.Parse(csv, "s", "1", "r");
        Assert.NotNull(dataset);
        Assert.Equal(3, errors.Count);                    // 逐行错误清单一一对应
        Assert.Equal(26, dataset!.Fields["ok"]["temp"].Value, 6);   // last-wins：最后一行覆盖
        Assert.False(dataset.Fields.ContainsKey("bad")); // 坏行全部跳过
    }

    [Fact]
    public void Csv_MissingHeaderColumn_RejectsWholeFile()
    {
        var (dataset, errors) = SolverCsvAdapter.Parse("node,value\na,1\n", "s", "1", "r");
        Assert.Null(dataset);                              // 结构性错误整体拒绝
        Assert.Contains(errors, e => e.Contains("field"));
    }

    [Fact]
    public void Dataset_RoundTrip_AndFutureSchemaRejected()
    {
        var (dataset, _) = SolverCsvAdapter.Parse(ReferenceCsv, "fea", "1.2", "run-42");
        var json = dataset!.Serialize();
        var (loaded, err) = SimResultDataset.Load(json);
        Assert.Null(err);
        Assert.NotNull(loaded);
        Assert.Equal(2.5e6, loaded!.Fields["beam-01"]["stress"].Value, 6);
        Assert.Equal(dataset.Fields.Count, loaded.Fields.Count);

        var (future, futureErr) = SimResultDataset.Load(
            json.Replace("\"schemaVersion\": 1", "\"schemaVersion\": 99"));
        Assert.Null(future);
        Assert.Contains("不静默覆写", futureErr);
    }

    [Fact]
    public void Units_InconsistencyDetected_NotSilentlyFixed()
    {
        var csv =
            "node,field,value,unit\n" +
            "a,stress,1000,Pa\n" +
            "b,stress,2,kPa\n" +       // 同名字段单位不一致
            "a,temp,20,degC\n" +
            "b,temp,21,degC\n";
        var (dataset, _) = SolverCsvAdapter.Parse(csv, "s", "1", "r");
        var issues = dataset!.UnitInconsistencies();
        var issue = Assert.Single(issues);
        Assert.Contains("stress", issue);
        Assert.Contains("Pa", issue);
        Assert.Contains("kPa", issue);
    }

    [Fact]
    public void Units_CrossUnitRead_UsesAffineTable()
    {
        var csv =
            "node,field,value,unit\n" +
            "pump,pressure,250,kPa\n";
        var (dataset, _) = SolverCsvAdapter.Parse(csv, "s", "1", "r");
        Assert.True(dataset!.TryRead("pump", "pressure", "bar", out var bar));
        Assert.Equal(2.5, bar, 10);                       // 250 kPa = 2.5 bar
        Assert.False(dataset.TryRead("pump", "pressure", "degC", out _));   // 未知对
        Assert.False(dataset.TryRead("pump", "missing", "bar", out _));     // 字段缺失
    }
}
