using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public sealed class DirtyPropagationTests
{
    [Fact]
    public void WorldEditorViewModel_AddScene_MarksProjectDirty()
    {
        using var temp = new TemporaryProject();
        var project = Project.CreateNew("p", temp.Path);
        project.Scenes.Add(new Scene("s"));
        project.ClearDirty();
        var vm = new WorldEditorViewModel { Project = project };
        Assert.False(project.IsDirty);
        vm.AddScene("added");
        Assert.True(project.IsDirty);
    }

    [Fact]
    public void WorldEditorViewModel_RemoveScene_MarksProjectDirty()
    {
        using var temp = new TemporaryProject();
        var project = Project.CreateNew("p", temp.Path);
        var scene = new Scene("s");
        project.Scenes.Add(scene);
        project.Scenes.Add(new Scene("s2"));
        var vm = new WorldEditorViewModel { Project = project };
        project.ClearDirty();
        vm.RemoveScene(scene);
        Assert.True(project.IsDirty);
    }

    [Fact]
    public void WorldEditorViewModel_AddEntity_MarksProjectDirty()
    {
        using var temp = new TemporaryProject();
        var project = Project.CreateNew("p", temp.Path);
        var scene = new Scene("s");
        project.Scenes.Add(scene);
        var vm = new WorldEditorViewModel { Project = project };
        project.ClearDirty();
        var ent = new GameEntity("e") { ParentScene = scene };
        ent.Components.Add(new Transform());
        vm.AddEntity(ent);
        Assert.True(project.IsDirty);
    }

    [Fact]
    public void WorldEditorViewModel_RemoveEntity_MarksProjectDirty()
    {
        using var temp = new TemporaryProject();
        var project = Project.CreateNew("p", temp.Path);
        var scene = new Scene("s");
        var ent = new GameEntity("e") { ParentScene = scene };
        ent.Components.Add(new Transform());
        scene.Entities.Add(ent);
        project.Scenes.Add(scene);
        var vm = new WorldEditorViewModel { Project = project };
        project.ClearDirty();
        vm.RemoveEntity(ent);
        Assert.True(project.IsDirty);
    }
}
