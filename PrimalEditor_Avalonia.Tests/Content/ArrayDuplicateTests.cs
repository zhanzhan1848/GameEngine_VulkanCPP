using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

/// <summary>
/// A5 Array 批量布设：深拷数量、沿轴位移步进、undo 单事务语义。
/// </summary>
public sealed class ArrayDuplicateTests
{
    private static (WorldEditorViewModel vm, Scene scene, GameEntity source) Arrange()
    {
        var project = new Project();
        var scene = new Scene("Main");
        project.Scenes.Add(scene);
        var source = new GameEntity("Block");
        source.Components.Add(new Transform { PositionX = 0f, PositionY = 1f, PositionZ = 2f });
        var child = new GameEntity("Child");
        child.Components.Add(new Transform { PositionX = 0.5f });
        source.Children.Add(child);
        child.Parent = source;
        scene.Entities.Add(source);
        var vm = new WorldEditorViewModel { Project = project, ActiveScene = scene };
        vm.SelectedEntities = new[] { source };
        return (vm, scene, source);
    }

    [Fact]
    public void ArrayDuplicate_CreatesSteppedClonesAlongAxis()
    {
        var (vm, scene, source) = Arrange();

        var created = vm.ArrayDuplicateSelected("X", 3, 1f);

        Assert.Equal(3, created);
        // 原实体 + 3 个根拷贝（子级不进 scene.Entities）。
        Assert.Equal(4, scene.Entities.Count);
        var xs = scene.Entities
            .Select(e => (name: e.Name, x: e.GetTransform()!.PositionX))
            .OrderBy(t => t.x)
            .ToArray();
        Assert.Equal(0f, xs[0].x);
        Assert.Equal(1f, xs[1].x);
        Assert.Equal(2f, xs[2].x);
        Assert.Equal(3f, xs[3].x);
        // 深拷含子级，且子级相对偏移保持。
        var last = scene.Entities.Single(e => e.GetTransform()!.PositionX == 3f);
        Assert.Equal(3.5f, last.Children.Single().GetTransform()!.PositionX);
        // 新 guid：实例不与源共享身份。
        Assert.All(scene.Entities.Where(e => e != source), e => Assert.NotEqual(source.EntityGuid, e.EntityGuid));
    }

    [Fact]
    public void ArrayDuplicate_UndoRemovesWholeBatch()
    {
        var (vm, scene, _) = Arrange();

        vm.ArrayDuplicateSelected("Y", 2, 1f);
        Assert.Equal(3, scene.Entities.Count);

        vm.UndoRedo.Undo();
        Assert.Equal(1, scene.Entities.Count);
    }
}
