using System.Linq;
using PrimalEditor_Avalonia.Common;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Common;

// A8: `git status --porcelain=v1` 输出的解析器单元测试（纯函数，无进程调用）。
public sealed class GitServicePorcelainTests
{
    [Fact]
    public void MixedStatuses_ParseToExpectedKinds()
    {
        var stdout =
            " M Scenes/Main.primal_scene\n" +
            "?? Content/new.particle.json\n" +
            "A  Content/added.mat\n" +
            "D  Content/gone.txt\n" +
            "MM Content/twice.mat\n";

        var entries = GitService.ParsePorcelain(stdout);

        Assert.Equal(5, entries.Count);
        Assert.Equal(GitService.FileStatus.Modified,
            entries.First(e => e.RepoRelativePath == "Scenes/Main.primal_scene").Status);
        Assert.Equal(GitService.FileStatus.Untracked,
            entries.First(e => e.RepoRelativePath == "Content/new.particle.json").Status);
        Assert.Equal(GitService.FileStatus.Added,
            entries.First(e => e.RepoRelativePath == "Content/added.mat").Status);
        Assert.Equal(GitService.FileStatus.Deleted,
            entries.First(e => e.RepoRelativePath == "Content/gone.txt").Status);
        Assert.Equal(GitService.FileStatus.Modified,
            entries.First(e => e.RepoRelativePath == "Content/twice.mat").Status);
    }

    [Fact]
    public void RenameEntry_TracksNewPathOnly()
    {
        var entries = GitService.ParsePorcelain("R  Content/old.mat -> Content/new.mat\n");
        var entry = Assert.Single(entries);
        Assert.Equal("Content/new.mat", entry.RepoRelativePath);
    }

    [Fact]
    public void QuotedPathsWithSpaces_AreUnquoted()
    {
        var entries = GitService.ParsePorcelain("?? \"Content/my preset.particle.json\"\n");
        var entry = Assert.Single(entries);
        Assert.Equal("Content/my preset.particle.json", entry.RepoRelativePath);
        Assert.Equal(GitService.FileStatus.Untracked, entry.Status);
    }

    [Fact]
    public void EmptyAndShortLines_AreSkipped()
    {
        var entries = GitService.ParsePorcelain("\n\r\nab\n");
        Assert.Empty(entries);
    }
}
