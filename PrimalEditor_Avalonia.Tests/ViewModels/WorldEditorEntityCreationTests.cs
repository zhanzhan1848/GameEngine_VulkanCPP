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

	[Fact]
	public void DeleteSelectedEntity_RemovesImmediatelyAndSupportsUndoRedo()
	{
		var project = Project.CreateNew("Test", "/tmp/test-project");
		var scene = new Scene("Main");
		project.Scenes.Add(scene);
		var vm = new WorldEditorViewModel { Project = project, ActiveScene = scene };
		var entity = vm.CreateEntityFromFactory(EntityFactory.Find(EntityKind.Empty)!)!;

		Assert.Equal(1, vm.DeleteSelectedEntities());
		Assert.DoesNotContain(entity, scene.Entities);
		Assert.DoesNotContain(entity, vm.Entities);
		Assert.Empty(vm.SelectedEntities);

		vm.UndoRedo.Undo();
		Assert.Contains(entity, scene.Entities);
		Assert.Contains(entity, vm.Entities);

		vm.UndoRedo.Redo();
		Assert.DoesNotContain(entity, scene.Entities);
		Assert.DoesNotContain(entity, vm.Entities);
	}

	[Fact]
	public void UndoDeletedChild_RestoresItsParent()
	{
		var project = Project.CreateNew("Test", "/tmp/test-project");
		var scene = new Scene("Main");
		project.Scenes.Add(scene);
		var vm = new WorldEditorViewModel { Project = project, ActiveScene = scene };
		var parent = vm.CreateEntityFromFactory(EntityFactory.Find(EntityKind.Empty)!)!;
		var child = new PrimalEditor_Avalonia.Components.GameEntity("Child") { Parent = parent, ParentScene = scene };
		vm.AddEntity(child);
		vm.SelectedEntities = new[] { child };

		vm.DeleteSelectedEntities();
		Assert.DoesNotContain(child, parent.Children);

		vm.UndoRedo.Undo();
		Assert.Same(parent, child.Parent);
		Assert.Contains(child, parent.Children);
	}
}
