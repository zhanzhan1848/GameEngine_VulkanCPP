using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Common;

// A8: GitService 在真实临时 git 仓库上的端到端链路（探测/状态/提交）。
// 测试进程可直接调 git CLI（与编辑器同机制）。
public sealed class GitServiceEndToEndTests : IDisposable
{
    private readonly string _repo;

    public GitServiceEndToEndTests()
    {
        _repo = Path.Combine(Path.GetTempPath(), $"a8_git_{Guid.NewGuid():N}");
        Directory.CreateDirectory(_repo);
        Git("init", "--quiet");
        Git("config", "user.name", "test");
        Git("config", "user.email", "test@test");
        File.WriteAllText(Path.Combine(_repo, "scene.primal_scene"), "v1\n");
        Git("add", ".");
        Git("commit", "--quiet", "-m", "init");
    }

    public void Dispose()
    {
        try { Directory.Delete(_repo, recursive: true); } catch { /* Windows file locks */ }
    }

    /// <summary>macOS 的 /var、/tmp 是指向 /private/* 的中间组件符号链接
    /// （git 返回物理路径），DirectoryInfo.LinkTarget 只解析末段——按前缀
    /// 剥离 + 小写比较（macOS 文件系统不区分大小写）。</summary>
    internal static string Canonical(string path)
    {
        var p = Path.GetFullPath(path).Replace('\\', '/').TrimEnd('/');
        if (p.StartsWith("/private/var/", StringComparison.OrdinalIgnoreCase))
            p = "/var/" + p["/private/var/".Length..];
        if (p.StartsWith("/private/tmp/", StringComparison.OrdinalIgnoreCase))
            p = "/tmp/" + p["/private/tmp/".Length..];
        return p.ToLowerInvariant();
    }

    private (bool ok, string output) Git(params string[] args)
    {
        var psi = new System.Diagnostics.ProcessStartInfo
        {
            FileName = "git",
            WorkingDirectory = _repo,
            Arguments = string.Join(' ', args.Select(a => a.Contains(' ') ? $"\"{a}\"" : a)),
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        var p = System.Diagnostics.Process.Start(psi)!;
        p.WaitForExit();
        return (p.ExitCode == 0, p.StandardOutput.ReadToEnd());
    }

    [Fact]
    public void ProbeRepo_DetectsRepoAndRoot()
    {
        var state = GitService.ProbeRepo(_repo);
        Assert.True(state.IsRepo);
        // macOS /var → /private/var 符号链接：git 返回物理路径，比较需先规范化
        Assert.Equal(Canonical(_repo), Canonical(state.RepoRoot), ignoreCase: true);
    }

    [Fact]
    public void ProbeRepo_OutsideRepo_ReturnsNotRepo()
    {
        // /tmp 通常不是仓库；即真是，探测 false 分支也已被 ProbeRepo_Detects 覆盖
        var state = GitService.ProbeRepo(Path.Combine(Path.GetTempPath(), $"a8_no_repo_{Guid.NewGuid():N}"));
        Assert.False(state.IsRepo);
    }

    [Fact]
    public void Status_ModifiedAndUntracked_ParseCorrectly()
    {
        File.WriteAllText(Path.Combine(_repo, "scene.primal_scene"), "v2\n");
        File.WriteAllText(Path.Combine(_repo, "new.particle.json"), "{}\n");

        var files = GitService.Status(_repo);

        Assert.Contains(files, f => f.RepoRelativePath == "scene.primal_scene"
            && f.Status == GitService.FileStatus.Modified);
        Assert.Contains(files, f => f.RepoRelativePath == "new.particle.json"
            && f.Status == GitService.FileStatus.Untracked);
    }

    [Fact]
    public void Commit_SelectedPaths_CommitsAndCleansStatus()
    {
        File.WriteAllText(Path.Combine(_repo, "scene.primal_scene"), "v2\n");
        File.WriteAllText(Path.Combine(_repo, "other.txt"), "keep unstaged\n");

        var (ok, feedback) = GitService.Commit(_repo,
            new[] { "scene.primal_scene" }, "A8 test commit");

        Assert.True(ok, feedback);
        Assert.Equal("committed", feedback);
        var (logOk, log) = Git("log", "-1", "--pretty=%s");
        Assert.True(logOk);
        Assert.Contains("A8 test commit", log);

        // 提交后该文件不再出现在状态里，other.txt 保持 untracked
        var files = GitService.Status(_repo);
        Assert.DoesNotContain(files, f => f.RepoRelativePath == "scene.primal_scene");
        Assert.Contains(files, f => f.RepoRelativePath == "other.txt");
    }

    [Fact]
    public void Commit_NothingToCommit_IsSoftSuccess()
    {
        var (ok, feedback) = GitService.Commit(_repo,
            new[] { "scene.primal_scene" }, "no-op commit");
        Assert.True(ok);
        Assert.Contains("nothing", feedback, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Commit_EmptyMessage_Rejected()
    {
        var (ok, feedback) = GitService.Commit(_repo, new[] { "scene.primal_scene" }, "");
        Assert.False(ok);
    }
}
