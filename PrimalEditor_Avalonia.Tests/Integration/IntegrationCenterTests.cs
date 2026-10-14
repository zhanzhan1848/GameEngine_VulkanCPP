using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.Cli;
using PrimalEditor_Avalonia.Integration;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.IntegrationNs;

/// <summary>
/// 集成中心 S1 契约测试（software-integrations-and-skills-S1-A01/A02/A03）：
///   A01 发现/缺失夹具 + 手动路径含中文空格 + 服务与 CLI 两入口结果一致；
///   A02 坏路径/空候选/远端/重复 ID 的类型化语义；
///   A03 缓存往返一致 + 损坏/未来版本拒绝（不静默覆写）。
/// Skill 清单：只读扫描、损坏跳过计数、目录缺失=空（"未扫描到"≠"不存在"）。
/// </summary>
public sealed class IntegrationCenterTests : IDisposable
{
    private sealed class FakeAdapter : ISoftwareAdapter
    {
        private readonly string[] _candidates;
        public FakeAdapter(string id, params string[] candidates)
        {
            Id = id;
            _candidates = candidates;
        }
        public string Id { get; }
        public string Category => "test";
        public bool IsRemote { get; init; }
        public int AdapterVersion => 1;
        public IReadOnlyList<string> StaticCandidates() => _candidates;
    }

    private readonly string _tmp = Directory.CreateTempSubdirectory("primal_integrations_").FullName;

    private string Touch(string relative, string content = "")
    {
        var path = Path.Combine(_tmp, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, content);
        return path;
    }

    private IntegrationCenter MakeCenter()
    {
        var center = new IntegrationCenter
        {
            CachePath = Path.Combine(_tmp, "cache", "integrations-cache.json"),
            SkillRootsProvider = () => Array.Empty<(SkillOrigin, string)>(),
        };
        return center;
    }

    // ---- A01 发现/缺失 + 中文空格路径 ----

    [Fact]
    public void Detect_FoundAndMissing_Fixtures()
    {
        var c = MakeCenter();
        // 候选路径含中文与空格（合同 A01 夹具要求）
        var found = Touch("带 空格/工具 中文/bin/fakeA");
        c.RegisterAdapter(new FakeAdapter("fakeA", found));
        c.RegisterAdapter(new FakeAdapter("fakeB", Path.Combine(_tmp, "不存在 的 路径", "fakeB")));

        var records = c.DetectAll();
        var a = records.Single(r => r.SoftwareId == "fakeA");
        var b = records.Single(r => r.SoftwareId == "fakeB");
        Assert.Equal(DiscoveryStatus.Discovered, a.Status);
        Assert.Equal(found, a.ResolvedPath);
        Assert.Equal("candidate", a.Source);
        Assert.Null(a.Version);   // S1 不启动进程读版本——未知用 null
        Assert.Equal(DiscoveryStatus.NotFound, b.Status);
        Assert.Null(b.ResolvedPath);
    }

    [Fact]
    public void Detect_ManualOverride_WinsAndChinesePathWorks()
    {
        var c = MakeCenter();
        var auto = Touch("auto/fakeA");
        var manual = Touch("手动 目录/工具 A");   // 中文+空格手动路径
        c.RegisterAdapter(new FakeAdapter("fakeA", auto));

        var records = c.DetectAll(new Dictionary<string, string> { ["fakeA"] = manual });
        var a = records.Single(r => r.SoftwareId == "fakeA");
        Assert.Equal(DiscoveryStatus.Discovered, a.Status);
        Assert.Equal(manual, a.ResolvedPath);   // 显式选择不被自动发现覆盖
        Assert.Equal("manual", a.Source);
    }

    [Fact]
    public void Detect_StableOrdering_And_TwoEntries_Consistent()
    {
        var c = MakeCenter();
        var p1 = Touch("x/fakeZ");
        var p2 = Touch("x/fakeA");
        c.RegisterAdapter(new FakeAdapter("fakeZ", p1));
        c.RegisterAdapter(new FakeAdapter("fakeA", p2));

        var first = c.DetectAll();
        var second = c.DetectAll();
        Assert.Equal(first.Select(r => r.SoftwareId), second.Select(r => r.SoftwareId));
        Assert.Equal(new[] { "fakeA", "ffmpeg", "fakeZ" }.OrderBy(x => x, StringComparer.Ordinal),
            first.Select(r => r.SoftwareId));   // 含内置 ffmpeg，Ordinal 稳定序
    }

    // ---- A02 类型化失败语义 ----

    [Fact]
    public void Detect_BadManualPath_IsConfigMissing_NotSilent()
    {
        var c = MakeCenter();
        c.RegisterAdapter(new FakeAdapter("fakeA"));
        var records = c.DetectAll(new Dictionary<string, string>
        {
            ["fakeA"] = Path.Combine(_tmp, "坏 路径", "missing"),
        });
        var a = records.Single(r => r.SoftwareId == "fakeA");
        Assert.Equal(DiscoveryStatus.ConfigMissing, a.Status);
        Assert.Null(a.ResolvedPath);
    }

    [Fact]
    public void Detect_RemoteAdapter_IsUnknown_NotMissing()
    {
        var c = MakeCenter();
        c.RegisterAdapter(new FakeAdapter("cloudX") { IsRemote = true });
        // 远端服务 S1 不做连通探测：Unknown ≠ NotFound（"未验证"而非"缺失"）
        var records = c.DetectAll();
        var x = records.Single(r => r.SoftwareId == "cloudX");
        Assert.Equal(DiscoveryStatus.Unknown, x.Status);
    }

    [Fact]
    public void Register_DuplicateAdapterId_Throws()
    {
        var c = MakeCenter();
        c.RegisterAdapter(new FakeAdapter("dup"));
        Assert.Throws<InvalidOperationException>(() => c.RegisterAdapter(new FakeAdapter("dup")));
    }

    // ---- A03 缓存 ----

    [Fact]
    public void Cache_RoundTrip_PreservesRecords()
    {
        var c = MakeCenter();
        var found = Touch("bin/fakeA");
        c.RegisterAdapter(new FakeAdapter("fakeA", found));

        var (inventory, err) = c.RefreshAndSave();
        Assert.Null(err);
        Assert.True(File.Exists(c.CachePath));

        var (loaded, loadErr) = c.LoadCache();
        Assert.Null(loadErr);
        Assert.NotNull(loaded);
        var a = loaded!.Software.Single(r => r.SoftwareId == "fakeA");
        Assert.Equal(DiscoveryStatus.Discovered, a.Status);
        Assert.Equal(found, a.ResolvedPath);
        Assert.Equal(inventory.Software.Count, loaded.Software.Count);
    }

    [Fact]
    public void Cache_Corrupt_ReturnsTypedError()
    {
        var c = MakeCenter();
        Directory.CreateDirectory(Path.GetDirectoryName(c.CachePath)!);
        File.WriteAllText(c.CachePath, "{ not json !!!");
        var (loaded, err) = c.LoadCache();
        Assert.Null(loaded);
        Assert.NotNull(err);
        Assert.Contains("损坏", err);
    }

    [Fact]
    public void Cache_FutureSchemaVersion_RejectedNoSilentOverwrite()
    {
        var c = MakeCenter();
        Directory.CreateDirectory(Path.GetDirectoryName(c.CachePath)!);
        File.WriteAllText(c.CachePath,
            JsonSerializer.Serialize(new IntegrationInventory { SchemaVersion = 99 }));
        var (loaded, err) = c.LoadCache();
        Assert.Null(loaded);
        Assert.NotNull(err);
        Assert.Contains("不静默覆写", err);
    }

    // ---- Skill 清单 ----

    [Fact]
    public void Skills_ScanReadonly_SkipsCorrupt_MissingDirIsEmpty()
    {
        var userRoot = Path.Combine(_tmp, "skills-user");
        Directory.CreateDirectory(userRoot);
        File.WriteAllText(Path.Combine(userRoot, "encode.skill.json"),
            """{"skillId":"encode.clip","description":"帧序列→视频","requires":["ffmpeg"]}""");
        File.WriteAllText(Path.Combine(userRoot, "broken.skill.json"), "{ broken");
        var projectRoot = Path.Combine(_tmp, "Skills");   // 项目级
        Directory.CreateDirectory(projectRoot);
        File.WriteAllText(Path.Combine(projectRoot, "nav.patrol.skill.json"),
            """{"skillId":"nav.patrol","description":"巡逻脚本模板","requires":[]}""");

        var c = MakeCenter();
        c.SkillRootsProvider = () => new[]
        {
            (SkillOrigin.User, userRoot),
            (SkillOrigin.Project, projectRoot),
            (SkillOrigin.User, Path.Combine(_tmp, "目录 不存在")),   // 缺失=该来源空
        };
        var (inv, err) = c.RefreshAndSave();
        Assert.Null(err);

        var ids = inv.Skills.Select(s => s.SkillId).ToList();
        Assert.Equal(new[] { "encode.clip", "nav.patrol" }, ids);   // Ordinal 稳定序
        Assert.Equal(SkillOrigin.User, inv.Skills[0].Origin);
        Assert.Equal(SkillOrigin.Project, inv.Skills[1].Origin);
        Assert.Equal("unknown", inv.Skills[0].LastValidation);       // 有 Skill ≠ 已验证
        Assert.Single(c.LastSkippedSkillFiles, f => f.EndsWith("broken.skill.json"));
    }

    // ---- CLI 入口（与共享服务同结果——合同"UI/CLI 同能力"）----

    [Fact]
    public void Cli_List_NoCache_ReturnsUnknownSkeletonWithHint()
    {
        var c = MakeCenter();
        var cmd = new IntegrationCommands(c);
        dynamic result = cmd.List(refresh: false);
        Assert.Equal("empty", (string)result.source);
        Assert.Contains("refresh", (string)result.hint);
    }

    [Fact]
    public void Cli_Refresh_Matches_Service_Detection()
    {
        var c = MakeCenter();
        var found = Touch("工具 目录/fakeA");
        c.RegisterAdapter(new FakeAdapter("fakeA", found));
        c.RegisterAdapter(new FakeAdapter("fakeB", Path.Combine(_tmp, "missing", "fakeB")));

        var cmd = new IntegrationCommands(c);
        dynamic result = cmd.List(refresh: true);
        Assert.Equal("refresh", (string)result.source);

        var serviceRecords = c.DetectAll().ToDictionary(r => r.SoftwareId);
        foreach (dynamic s in result.software)
        {
            var rec = serviceRecords[(string)s.SoftwareId];
            var expected = rec.Status switch
            {
                DiscoveryStatus.NotFound => "not-found",
                DiscoveryStatus.ConfigMissing => "config-missing",
                DiscoveryStatus.Discovered => "discovered",
                _ => "unknown",
            };
            Assert.Equal(expected, (string)s.status);
            Assert.Equal(rec.ResolvedPath, (string?)s.resolvedPath);
        }
        // ffmpeg 在本机是否存在不影响断言：只锁 fakeA/fakeB 的语义
        var fakeA = ((IEnumerable<dynamic>)result.software).Single(x => (string)x.SoftwareId == "fakeA");
        var fakeB = ((IEnumerable<dynamic>)result.software).Single(x => (string)x.SoftwareId == "fakeB");
        Assert.Equal("discovered", (string)fakeA.status);
        Assert.Equal("not-found", (string)fakeB.status);
    }

    public void Dispose()
    {
        try { Directory.Delete(_tmp, recursive: true); } catch { }
    }
}
