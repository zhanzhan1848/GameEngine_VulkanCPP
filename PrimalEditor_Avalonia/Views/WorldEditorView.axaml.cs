using System.Linq;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class WorldEditorView : UserControl
{
    public WorldEditorView()
    {
        InitializeComponent();
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private void OnAddSceneClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm)
        {
            vm.AddScene();
        }
    }

    private void OnDeleteSceneClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm)
        {
            vm.RemoveScene(vm.ActiveScene);
        }
    }

    private void OnHierarchyFilterChanged(object? sender, Avalonia.Controls.TextChangedEventArgs e)
    {
        if (DataContext is not WorldEditorViewModel vm) return;
        var filter = this.FindControl<Avalonia.Controls.TextBox>("HierarchyFilter")?.Text ?? "";
        vm.ApplyFilter(filter);
    }

    private void OnTreeSelectionChanged(object? sender, SelectionChangedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm && sender is TreeView treeView)
        {
            var selected = treeView.SelectedItems.OfType<GameEntity>().ToArray();
            vm.SelectedEntities = selected;
            if (vm.Properties != null)
            {
                vm.Properties.Entities = selected;
            }
        }
    }

    private void OnAddEntityClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm)
        {
            if (vm.ActiveScene == null)
            {
                Logger.Warning("Cannot add entity: no active scene. Open a project first.");
                return;
            }

            var parent = vm.SelectedEntities.Length == 1 ? vm.SelectedEntities[0] : null;
            var entityName = parent != null
                ? $"{parent.Name}_Child_{parent.Children.Count + 1}"
                : $"Entity_{vm.Entities.Count + 1}";

            var entity = new GameEntity(entityName);
            entity.Components.Add(new Transform());

            if (parent != null)
            {
                parent.Children.Add(entity);
                entity.Parent = parent;
            }
            else
            {
                vm.ActiveScene.Entities.Add(entity);
                entity.ParentScene = vm.ActiveScene;
            }

            vm.UndoRedo.Add(new AddEntityAction(vm, entity));
            vm.SelectedEntities = new[] { entity };
            Logger.Info($"Added entity: {entity.Name}" + (parent != null ? $" (child of {parent.Name})" : ""));
        }
    }

    private void OnDeleteEntityClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm && vm.SelectedEntities.Length > 0)
        {
            int count = 0;
            foreach (var entity in vm.SelectedEntities.ToList())
            {
                CountRecursive(entity, ref count);
                RemoveRecursive(entity);
                vm.UndoRedo.Add(new RemoveEntityAction(vm, entity));
                ((RemoveEntityAction)vm.UndoRedo.UndoStack[^1]).Redo();
            }
            vm.SelectedEntities = System.Array.Empty<GameEntity>();
            Logger.Info($"Deleted {count} entity(ies) (including children)");
        }
    }

    private int CountRecursive(GameEntity entity, ref int count)
    {
        count++;
        foreach (var child in entity.Children)
        {
            CountRecursive(child, ref count);
        }
        return count;
    }

    private void RemoveRecursive(GameEntity entity)
    {
        if (entity.Parent != null)
        {
            entity.Parent.Children.Remove(entity);
        }
        else if (entity.ParentScene != null)
        {
            entity.ParentScene.Entities.Remove(entity);
        }
        entity.Parent = null;
        entity.ParentScene = null;
    }

    private void OnEntityDoubleClick(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        if (DataContext is WorldEditorViewModel vm
            && this.FindControl<TreeView>("HierarchyTree") is { } tree
            && tree.SelectedItem is GameEntity entity)
        {
            var dialog = new RenameEntityDialog(entity.Name);
            var topLevel = TopLevel.GetTopLevel(this);
            if (topLevel is Window window)
            {
                var result = dialog.ShowDialog<string?>(window);
                result.Wait();
                if (!string.IsNullOrEmpty(result.Result) && result.Result != entity.Name)
                {
                    var action = new RenameEntityAction(entity, result.Result);
                    vm.UndoRedo.Add(action);
                    action.Redo();
                    Logger.Info($"Renamed entity to: {result.Result}");
                }
            }
        }
    }
}