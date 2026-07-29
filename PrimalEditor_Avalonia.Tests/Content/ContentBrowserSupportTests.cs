using System.IO;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class AssetSearchFilterTests
{
    [Fact]
    public void EmptyQuery_MatchesEverything()
    {
        Assert.True(AssetSearchFilter.Match("anything.png", ""));
        Assert.True(AssetSearchFilter.Match("anything.png", null));
    }

    [Fact]
    public void EmptyName_DoesNotMatchNonEmptyQuery()
    {
        Assert.False(AssetSearchFilter.Match(null, "x"));
        Assert.False(AssetSearchFilter.Match("", "x"));
    }

    [Fact]
    public void CaseInsensitiveSubstringMatch()
    {
        Assert.True(AssetSearchFilter.Match("MyTexture.PNG", "texture"));
        Assert.True(AssetSearchFilter.Match("MyTexture.PNG", "PNG"));
        Assert.True(AssetSearchFilter.Match("MyTexture.PNG", "mytex"));
        Assert.False(AssetSearchFilter.Match("MyTexture.PNG", "obj"));
    }
}

public sealed class BreadcrumbBuilderTests
{
    [Fact]
    public void RootOnly_WhenNoSelection()
    {
        var crumbs = BreadcrumbBuilder.Build("/proj/Content", null);
        Assert.Single(crumbs);
        Assert.True(crumbs[0].IsRoot);
        Assert.Equal("/proj/Content", crumbs[0].FullPath);
    }

    [Fact]
    public void RootOnly_WhenSelectionEqualsRoot()
    {
        var crumbs = BreadcrumbBuilder.Build("/proj/Content", "/proj/Content");
        Assert.Single(crumbs);
        Assert.Equal("/proj/Content", crumbs[0].FullPath);
    }

    [Fact]
    public void NestedFolders_ProduceOneSegmentPerLevel()
    {
        var crumbs = BreadcrumbBuilder.Build("/proj/Content", "/proj/Content/Textures/Heroes");
        Assert.Equal(3, crumbs.Count);
        Assert.Equal("Content", crumbs[0].Name);
        Assert.True(crumbs[0].IsRoot);
        Assert.Equal("Textures", crumbs[1].Name);
        Assert.Equal("/proj/Content/Textures", crumbs[1].FullPath);
        Assert.Equal("Heroes", crumbs[2].Name);
        Assert.Equal("/proj/Content/Textures/Heroes", crumbs[2].FullPath);
    }

    [Fact]
    public void OutOfRangeSelection_ReturnsOnlyRoot()
    {
        var crumbs = BreadcrumbBuilder.Build("/proj/Content", "/somewhere/Else");
        Assert.Single(crumbs);
        Assert.True(crumbs[0].IsRoot);
    }
}
