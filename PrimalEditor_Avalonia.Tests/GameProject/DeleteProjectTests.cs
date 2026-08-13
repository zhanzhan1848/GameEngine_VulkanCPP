using System;
using System.IO;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.GameProject;

public sealed class DeleteProjectTests
{
    private static ProjectTemplate AnyTemplate() =>
        new("Empty Project", "A new empty project with no scenes", "EmptyProject");

    [Fact]
    public void DeleteProjectFolder_RemovesEntireProjectDirectory()
    {
        using var parent = new TemporaryDirectory();
        var project = NewProject.Create("MyGame", parent.Path, AnyTemplate());
        var root = project.Path;
        Assert.True(Directory.Exists(Path.Combine(root, "Content")));

        var deleted = DeleteProject.DeleteProjectFolder(project.FullPath);

        Assert.True(deleted);
        Assert.False(Directory.Exists(root));
        Assert.False(File.Exists(project.FullPath));
    }

    [Fact]
    public void DeleteProjectFolder_LeavesSiblingProjectsUntouched()
    {
        using var parent = new TemporaryDirectory();
        var first = NewProject.Create("Alpha", parent.Path, AnyTemplate());
        var second = NewProject.Create("Beta", parent.Path, AnyTemplate());

        DeleteProject.DeleteProjectFolder(first.FullPath);

        Assert.False(Directory.Exists(first.Path));
        Assert.True(Directory.Exists(second.Path));
        Assert.True(File.Exists(second.FullPath));
        Assert.True(Directory.Exists(parent.Path));
    }

    [Fact]
    public void DeleteProjectFolder_DeletesNestedContent()
    {
        using var parent = new TemporaryDirectory();
        var project = NewProject.Create("MyGame", parent.Path, AnyTemplate());
        var nested = Path.Combine(project.Path, "Content", "Meshes", "Deep");
        Directory.CreateDirectory(nested);
        File.WriteAllText(Path.Combine(nested, "mesh.asset"), "payload");

        DeleteProject.DeleteProjectFolder(project.FullPath);

        Assert.False(Directory.Exists(project.Path));
    }

    [Fact]
    public void ResolveProjectRoot_ReturnsFolderContainingProjectFile()
    {
        using var parent = new TemporaryDirectory();
        var project = NewProject.Create("MyGame", parent.Path, AnyTemplate());

        var root = DeleteProject.ResolveProjectRoot(project.FullPath);

        Assert.Equal(project.Path, root);
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    public void ResolveProjectRoot_RejectsEmptyPath(string path)
    {
        Assert.Null(DeleteProject.ResolveProjectRoot(path));
    }

    [Fact]
    public void ResolveProjectRoot_RejectsNonProjectExtension()
    {
        using var parent = new TemporaryDirectory();
        var stray = Path.Combine(parent.Path, "notes.txt");
        File.WriteAllText(stray, "important");

        Assert.Null(DeleteProject.ResolveProjectRoot(stray));
        Assert.True(File.Exists(stray));
    }

    [Fact]
    public void ResolveProjectRoot_RejectsMissingFile()
    {
        using var parent = new TemporaryDirectory();
        var ghost = Path.Combine(parent.Path, "Gone", "Gone.primal");

        Assert.Null(DeleteProject.ResolveProjectRoot(ghost));
    }

    [Fact]
    public void ResolveProjectRoot_RejectsHomeDirectory()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (string.IsNullOrEmpty(home) || !Directory.Exists(home))
        {
            return;
        }

        var marker = Path.Combine(home, $"primal-guard-{Guid.NewGuid():N}.primal");
        File.WriteAllText(marker, "{}");
        try
        {
            Assert.Null(DeleteProject.ResolveProjectRoot(marker));
        }
        finally
        {
            File.Delete(marker);
        }
    }

    [Fact]
    public void DeleteProjectFolder_ReturnsFalseWithoutThrowingForMissingProject()
    {
        using var parent = new TemporaryDirectory();
        var ghost = Path.Combine(parent.Path, "Gone", "Gone.primal");

        Assert.False(DeleteProject.DeleteProjectFolder(ghost));
        Assert.True(Directory.Exists(parent.Path));
    }
}
