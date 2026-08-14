using PrimalEditor_Avalonia.Entities;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

public sealed class WorldEditorEntityCreationTests
{
    [Fact]
    public void RepeatedFactoryCreation_AddsSiblingsAtSceneRoot()
    {
        var project = Project.CreateNew("Test", "/tmp/test-project");
        var scene = new Scene("Main");
        project.Scenes.Add(scene);
        var vm = new WorldEditorViewModel { Project = project, ActiveScene = scene };
        var descriptor = EntityFactory.Find(EntityKind.Empty)!;

        var first = vm.CreateEntityFromFactory(descriptor)!;
        var second = vm.CreateEntityFromFactory(descriptor)!;

        Assert.Equal(2, scene.Entities.Count);
        Assert.Same(first, scene.Entities[0]);
        Assert.Same(second, scene.Entities[1]);
        Assert.Null(first.Parent);
        Assert.Null(second.Parent);
        Assert.Same(scene, first.ParentScene);
        Assert.Same(scene, second.ParentScene);
        Assert.Empty(first.Children);
        Assert.Equal(new[] { second }, vm.SelectedEntities);
    }

    [Fact]
    public void UndoCreatedRoot_RemovesItFromSceneAndHierarchySources()
    {
        var project = Project.CreateNew("Test", "/tmp/test-project");
        var scene = new Scene("Main");
        project.Scenes.Add(scene);
        var vm = new WorldEditorViewModel { Project = project, ActiveScene = scene };
        var entity = vm.CreateEntityFromFactory(EntityFactory.Find(EntityKind.Empty)!)!;

        vm.UndoRedo.Undo();

        Assert.DoesNotContain(entity, scene.Entities);
        Assert.DoesNotContain(entity, vm.Entities);
    }
}
