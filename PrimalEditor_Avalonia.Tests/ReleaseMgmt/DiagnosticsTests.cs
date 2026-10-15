using System;
using System.Linq;
using PrimalEditor_Avalonia.ReleaseMgmt;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ReleaseMgmtNs;

/// <summary>
/// release-maintenance S3 契约测试：隐私字段过滤（home 路径/IP/密钥）、崩溃
/// 转储默认排除（可显式开启）、缺版本/构建 id 拒绝导出、空包拒绝、往返+
/// 未知版本拒；平台矩阵默认值与校验（重复/未知状态）。
/// </summary>
public sealed class DiagnosticsTests
{
    private static DiagnosticBundle Sample() => new()
    {
        EngineVersion = "0.9.1",
        AppVersion = "0.9.1",
        BuildId = "build-42",
        CreatedAtUtc = new DateTime(2026, 10, 15, 0, 0, 0, DateTimeKind.Utc),
        Entries =
        {
            new DiagnosticEntry
            {
                Kind = DiagnosticEntryKind.SystemInfo,
                Source = "sys.json",
                Content = "home=/Users/alice/Library logs=/Users/alice/Logs ip=192.168.1.10",
            },
            new DiagnosticEntry
            {
                Kind = DiagnosticEntryKind.UserNote,
                Source = "note.txt",
                Content = "登录后闪退，token=abc123DEF 后黑屏",
            },
            new DiagnosticEntry
            {
                Kind = DiagnosticEntryKind.CrashDump,
                Source = "crash.dmp",
                Content = "<binary memory>",
            },
            new DiagnosticEntry
            {
                Kind = DiagnosticEntryKind.EngineLog,
                Source = "engine.log",
                Content = "[RHI] adapter=Apple M1",
            },
        },
    };

    [Fact]
    public void Export_RedactsPrivacy_ExcludesDumpByDefault()
    {
        var (json, warnings) = DiagnosticExporter.Export(Sample());
        Assert.NotNull(json);

        var bundle = DiagnosticBundle.Load(json!).bundle!;
        Assert.DoesNotContain("alice", json);            // 用户名掩码
        Assert.DoesNotContain("192.168.1.10", json);     // IP 掩码
        Assert.DoesNotContain("abc123DEF", json);        // 密钥掩码
        Assert.Contains("[REDACTED]", json);
        Assert.DoesNotContain("crash.dmp", json);        // 转储默认排除
        Assert.Contains(warnings, w => w.Contains("crash dump 排除"));
        Assert.Contains(warnings, w => w.Contains("隐私字段已掩码"));
        // 引擎日志不误伤：非隐私内容原样保留
        Assert.Contains("Apple M1", json);
        // 追溯键保留
        Assert.Equal("0.9.1", bundle.EngineVersion);
        Assert.Equal("build-42", bundle.BuildId);
    }

    [Fact]
    public void Export_CrashDumpExplicitOptIn()
    {
        var (json, warnings) = DiagnosticExporter.Export(Sample(), includeCrashDumps: true);
        Assert.NotNull(json);
        Assert.Contains("crash.dmp", json);
        Assert.DoesNotContain(warnings, w => w.Contains("crash dump 排除"));
    }

    [Fact]
    public void Export_RejectsMissingIdentityAndEmpty()
    {
        var noVersion = Sample();
        noVersion.EngineVersion = "";
        var (json1, w1) = DiagnosticExporter.Export(noVersion);
        Assert.Null(json1);
        Assert.Contains(w1, w => w.Contains("拒绝导出"));

        var noBuild = Sample();
        noBuild.BuildId = "";
        var (json2, _) = DiagnosticExporter.Export(noBuild);
        Assert.Null(json2);

        // 全部条目被排除 → 空包拒绝
        var empty = Sample();
        empty.Entries.RemoveAll(e => e.Kind != DiagnosticEntryKind.CrashDump);
        var (json3, w3) = DiagnosticExporter.Export(empty);
        Assert.Null(json3);
        Assert.Contains(w3, w => w.Contains("空诊断包"));
    }

    [Fact]
    public void Bundle_RoundTrip_AndFutureSchemaRejected()
    {
        var (json, _) = DiagnosticExporter.Export(Sample());
        var (loaded, err) = DiagnosticBundle.Load(json!);
        Assert.Null(err);
        Assert.Equal(3, loaded!.Entries.Count);   // dump 排除后 3 条

        var (future, futureErr) = DiagnosticBundle.Load(
            json!.Replace("\"schemaVersion\": 1", "\"schemaVersion\": 8"));
        Assert.Null(future);
        Assert.Contains("不静默误读", futureErr);
    }

    [Fact]
    public void PlatformMatrix_DefaultAndValidation()
    {
        var matrix = PlatformMatrix.Default();
        Assert.Empty(matrix.Validate());
        Assert.Contains(matrix.Rows, r => r.Platform == "macOS-arm64" && r.Status == "supported");
        Assert.Contains(matrix.Rows, r => r.Status == "untested");   // 未测不虚标

        matrix.Rows.Add(new PlatformMatrix.Row { Platform = "macOS-arm64", Status = "gold" });
        var errors = matrix.Validate();
        Assert.Contains(errors, e => e.Contains("平台重复"));
        Assert.Contains(errors, e => e.Contains("未知状态 'gold'"));
    }
}
