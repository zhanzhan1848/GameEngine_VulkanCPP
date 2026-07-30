using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class ContentBrowserViewModelTests
{
    /// <summary>
    /// Construct a ContentBrowserViewModel with a real on-disk project
    /// root so the RefreshFolders/LoadFilesForSelectedFolder paths can
    /// run without mocking the filesystem. The temp directory is
    /// created in the ctor and removed in the IDisposable dispose —
    /// xUnit instantiates a fresh class per test so cleanup is reliable.
    /// </summary>
    private sealed class TestProject : System.IDisposable
    {
        public string Root { get; }
        public Project Project { get; }
        public ContentBrowserViewModel ViewModel { get; }

        public TestProject()
        {
            Root = Path.Combine(Path.GetTempPath(), "P1bTest_" + System.Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Root);
            Directory.CreateDirectory(Path.Combine(Root, "Content"));
            // Add a few asset files so the filtered list is non-empty.
            File.WriteAllText(Path.Combine(Root, "Content", "hero.png"), "fake");
            File.WriteAllText(Path.Combine(Root, "Content", "monster.png"), "fake");
            File.WriteAllText(Path.Combine(Root, "Content", "level.obj"), "fake");
            Project = new Project { Path = Root };
            ViewModel = new ContentBrowserViewModel { Project = Project };
        }

        public void Dispose()
        {
            try { Directory.Delete(Root, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SearchQuery_FiltersByFileName()
    {
        using var t = new TestProject();
        Assert.NotEmpty(t.ViewModel.FilteredFiles);

        t.ViewModel.SearchQuery = "hero";
        Assert.Single(t.ViewModel.FilteredFiles);
        Assert.Equal("hero.png", t.ViewModel.FilteredFiles[0].FileName);
    }

    [Fact]
    public void SearchQuery_IsCaseInsensitive()
    {
        using var t = new TestProject();
        t.ViewModel.SearchQuery = "HERO";
        Assert.Single(t.ViewModel.FilteredFiles);
        Assert.Equal("hero.png", t.ViewModel.FilteredFiles[0].FileName);
    }

    [Fact]
    public void SearchQuery_ClearSearch_RestoresAllFiles()
    {
        using var t = new TestProject();
        var total = t.ViewModel.FilteredFiles.Count;
        Assert.True(total >= 2);

        t.ViewModel.SearchQuery = "hero";
        Assert.Single(t.ViewModel.FilteredFiles);

        t.ViewModel.ClearSearch();
        Assert.Empty(t.ViewModel.SearchQuery);
        Assert.False(t.ViewModel.HasActiveSearch);
        Assert.Equal(total, t.ViewModel.FilteredFiles.Count);
    }

    [Fact]
    public void HasActiveSearch_TogglesOnOff()
    {
        using var t = new TestProject();
        Assert.False(t.ViewModel.HasActiveSearch);
        t.ViewModel.SearchQuery = "x";
        Assert.True(t.ViewModel.HasActiveSearch);
        t.ViewModel.SearchQuery = "";
        Assert.False(t.ViewModel.HasActiveSearch);
    }

    [Fact]
    public void ResultCountDisplay_EmptyWhenNoSearch()
    {
        using var t = new TestProject();
        Assert.Empty(t.ViewModel.ResultCountDisplay);
    }

    [Fact]
    public void ResultCountDisplay_FormatsWhenSearching()
    {
        using var t = new TestProject();
        t.ViewModel.SearchQuery = "png";
        // 2 PNG files in the fixture; FilteredFiles.Count should be 2.
        Assert.Equal($"2 of {t.ViewModel.Files.Count}", t.ViewModel.ResultCountDisplay);
    }

    [Fact]
    public void ResultCountDisplay_ShowsZeroOfTotalWhenNoMatch()
    {
        using var t = new TestProject();
        t.ViewModel.SearchQuery = "doesnotmatch";
        Assert.Equal($"0 of {t.ViewModel.Files.Count}", t.ViewModel.ResultCountDisplay);
    }

    [Fact]
    public void Breadcrumbs_ContainRootWhenProjectLoaded()
    {
        using var t = new TestProject();
        Assert.NotEmpty(t.ViewModel.Breadcrumbs);
        Assert.True(t.ViewModel.HasBreadcrumbs);
        Assert.True(t.ViewModel.Breadcrumbs[0].IsRoot);
    }

    [Fact]
    public void NavigateToBreadcrumb_Root_DoesNotChangeSelection()
    {
        using var t = new TestProject();
        var root = t.ViewModel.Breadcrumbs.First(s => s.IsRoot);
        var before = t.ViewModel.SelectedFolder?.FullPath;
        t.ViewModel.NavigateToBreadcrumb(root);
        Assert.Equal(before, t.ViewModel.SelectedFolder?.FullPath);
    }
}