using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// A8 源码控制最小集成：项目目录上的 git 只读探测 + 最小提交链。
/// 全部经 Process.Start 调系统 git（编辑器不嵌 git 库）；git 不可用或
/// 项目不在仓库内时各方法安全降级（IsRepo=false / 空列表 / false）。
/// </summary>
public static class GitService
{
    public enum FileStatus { None, Modified, Added, Deleted, Untracked }

    public sealed record RepoState(bool IsRepo, string RepoRoot);

    public sealed record FileEntry(string RepoRelativePath, FileStatus Status);

    /// <summary>Max seconds per git invocation; git never blocks the UI
    /// thread (callers go through Task.Run), but a hung process must not
    /// accumulate.</summary>
    private const int TimeoutMs = 15_000;

    /// <summary>Run git with the given args; returns (ok, stdout, stderr).
    /// Exit code 0 → ok. Never throws.</summary>
    public static (bool ok, string stdout, string stderr) Run(
        string workingDirectory, params string[] args)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "git",
                WorkingDirectory = string.IsNullOrEmpty(workingDirectory) ? "." : workingDirectory,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            foreach (var a in args) psi.ArgumentList.Add(a);

            using var p = Process.Start(psi);
            if (p == null) return (false, string.Empty, "process start failed");
            var stdout = p.StandardOutput.ReadToEnd();
            var stderr = p.StandardError.ReadToEnd();
            if (!p.WaitForExit(TimeoutMs))
            {
                try { p.Kill(true); } catch { /* best effort */ }
                return (false, stdout, "git timed out");
            }
            return (p.ExitCode == 0, stdout, stderr);
        }
        catch (Exception ex)
        {
            return (false, string.Empty, ex.Message);
        }
    }

    /// <summary>Probe whether the project lives inside a git work tree and
    /// capture the repo root (porcelain paths are root-relative, so every
    /// later call needs it).</summary>
    public static RepoState ProbeRepo(string projectPath)
    {
        if (string.IsNullOrEmpty(projectPath) || !Directory.Exists(projectPath))
            return new RepoState(false, string.Empty);
        var (ok, stdout, _) = Run(projectPath, "rev-parse", "--show-toplevel");
        if (!ok) return new RepoState(false, string.Empty);
        return new RepoState(true, stdout.Trim());
    }

    /// <summary>Parse `git status --porcelain=v1` into file entries, scoped
    /// to the project subtree ("-- ." from the project cwd) so a project
    /// living inside a bigger repo doesn't flood the panel. Untracked
    /// directories are expanded with `--untracked-files=all` so individual
    /// .particle.json / scene files show up, not just ".Primal/".</summary>
    public static List<FileEntry> Status(string projectPath)
    {
        if (string.IsNullOrEmpty(projectPath)) return new List<FileEntry>();
        var (ok, stdout, _) = Run(projectPath, "status", "--porcelain=v1",
            "--untracked-files=all", "--", ".");
        if (!ok) return new List<FileEntry>();
        return ParsePorcelain(stdout);
    }

    /// <summary>Pure porcelain v1 parser (unit-testable). Format per line:
    /// XY <space> path — X staged, Y worktree. "??" = untracked; renames
    /// carry "old -> new" and we track the new path.</summary>
    public static List<FileEntry> ParsePorcelain(string stdout)
    {
        var result = new List<FileEntry>();
        foreach (var raw in stdout.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.Length < 4) continue;
            var xy = line.Substring(0, 2);
            var path = line.Substring(3).Trim('"');
            if (path.Length == 0) continue;
            // Rename entries "R  old -> new": track the new path only.
            var arrow = path.IndexOf(" -> ", StringComparison.Ordinal);
            if (arrow >= 0) path = path[(arrow + 4)..];

            var status = xy switch
            {
                "??" => FileStatus.Untracked,
                var s when s.Contains('A') => FileStatus.Added,
                var s when s.Contains('D') => FileStatus.Deleted,
                var s when s.Contains('M') || s.Contains('T') => FileStatus.Modified,
                _ => FileStatus.Modified, // C/U/R and friends: surface as changed
            };
            result.Add(new FileEntry(path, status));
        }
        return result;
    }

    /// <summary>Stage the given repo-relative paths and commit with the
    /// message. Returns (success, feedback) where feedback carries the
    /// human-readable outcome ("nothing to commit" is a soft success with
    /// that message, not an error toast).</summary>
    public static (bool ok, string feedback) Commit(
        string projectPath, IReadOnlyList<string> repoRelativePaths, string message)
    {
        if (string.IsNullOrWhiteSpace(message)) return (false, "commit message is empty");
        if (repoRelativePaths.Count == 0) return (false, "no files selected");

        var (addOk, _, addErr) = Run(projectPath, ["add", "--", .. repoRelativePaths]);
        if (!addOk) return (false, $"git add failed: {addErr}");

        var (commitOk, commitOut, commitErr) = Run(projectPath, "commit", "-m", message);
        if (commitOk) return (true, "committed");

        // git exits 1 with "nothing to commit" when the staged snapshot
        // matches HEAD — and prints that banner to stdout (not stderr).
        if (commitOut.Contains("nothing to commit", StringComparison.OrdinalIgnoreCase) ||
            commitErr.Contains("nothing to commit", StringComparison.OrdinalIgnoreCase) ||
            commitErr.Contains("no changes added to commit", StringComparison.OrdinalIgnoreCase))
            return (true, "nothing to commit");

        return (false, $"git commit failed: {commitErr}{commitOut}");
    }
}
