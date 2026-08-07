using System;
using System.IO;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.GameProject;

public sealed class NewProjectTests
{
    private static ProjectTemplate AnyTemplate() =>
        new("Empty Project", "A new empty project with no scenes", "EmptyProject");

    [Fact]
    public void Create_PlacesProjectInDedicatedSubfolder()
    {
        using var parent = new TemporaryDirectory();

        var project = NewProject.Create("MyGame", parent.Path, AnyTemplate());

        var expectedRoot = Path.Combine(parent.Path, "MyGame");
        Assert.Equal(expectedRoot, project.Path);
        Assert.Equal("MyGame", project.Name);
        Assert.Equal(Path.Combine(expectedRoot, "MyGame.primal"), project.FullPath);
        Assert.True(Directory.Exists(expectedRoot));
        Assert.True(Directory.Exists(Path.Combine(expectedRoot, "Content")));
        Assert.True(Directory.Exists(Path.Combine(expectedRoot, ".Primal", "Temp")));
        Assert.True(File.Exists(project.FullPath));
    }

    [Fact]
    public void Create_DoesNotPolluteParentDirectory()
    {
        using var parent = new TemporaryDirectory();

        NewProject.Create("MyGame", parent.Path, AnyTemplate());

        // Parent must NOT contain stray Content/ or .Primal/ folders from
        // the new project — that was the original bug.
        Assert.False(Directory.Exists(Path.Combine(parent.Path, "Content")));
        Assert.False(Directory.Exists(Path.Combine(parent.Path, ".Primal")));
        Assert.Empty(Directory.GetDirectories(parent.Path, "Content"));
        Assert.Empty(Directory.GetDirectories(parent.Path, ".Primal"));
    }

    [Fact]
    public void Create_TwoProjectsInSameParentDoNotCollide()
    {
        using var parent = new TemporaryDirectory();

        var first = NewProject.Create("Alpha", parent.Path, AnyTemplate());
        var second = NewProject.Create("Beta", parent.Path, AnyTemplate());

        Assert.NotEqual(first.Path, second.Path);
        Assert.True(Directory.Exists(first.Path));
        Assert.True(Directory.Exists(second.Path));
        Assert.True(File.Exists(first.FullPath));
        Assert.True(File.Exists(second.FullPath));
    }

    [Fact]
    public void Create_RejectsPreExistingTargetFolder()
    {
        using var parent = new TemporaryDirectory();
        var preExisting = Path.Combine(parent.Path, "MyGame");
        Directory.CreateDirectory(preExisting);
        File.WriteAllText(Path.Combine(preExisting, "user-data.txt"), "do not delete");

        Action act = () => NewProject.Create("MyGame", parent.Path, AnyTemplate());
        var ex = Assert.Throws<IOException>(act);

        Assert.Contains("already exists", ex.Message);
        // Existing user data must be untouched.
        Assert.True(File.Exists(Path.Combine(preExisting, "user-data.txt")));
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("...")]
    [InlineData("..")]
    public void Create_RejectsEmptyOrTraversalNames(string name)
    {
        using var parent = new TemporaryDirectory();

        Action act = () => NewProject.Create(name, parent.Path, AnyTemplate());
        Assert.ThrowsAny<ArgumentException>(act);
    }

    [Fact]
    public void Create_StripsInvalidPathCharacters()
    {
        using var parent = new TemporaryDirectory();

        // "/" and ":" are path separators / illegal in Windows; question
        // mark and asterisk are illegal on Windows. Sanitizer replaces all
        // of them with '_'.
        var project = NewProject.Create("My/Game:Test*?", parent.Path, AnyTemplate());

        Assert.Equal(Path.Combine(parent.Path, "My_Game_Test__"), project.Path);
        Assert.Equal("My_Game_Test__", project.Name);
    }

    [Fact]
    public void Create_RequiresTemplate()
    {
        using var parent = new TemporaryDirectory();

        Action act = () => NewProject.Create("MyGame", parent.Path, null!);
        Assert.Throws<ArgumentNullException>(act);
    }
}