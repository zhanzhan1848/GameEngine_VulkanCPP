using Dock.Model.Core;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Docking;

public sealed class MainWindowDockToggleTests
{
    [Fact]
    public void ToggleWorldEditor_HidesDockable()
    {
        var vm = new MainWindowViewModel();
        // Capture the original document dock and ensure WorldEditor is there
        var root = vm.Layout;
        var docDock = Assert.IsType<Dock.Model.Mvvm.Controls.DocumentDock>(root!.VisibleDockables[0]);
        Assert.Contains(vm.WorldEditor, docDock.VisibleDockables);

        vm.IsWorldEditorVisible = false;

        Assert.DoesNotContain(vm.WorldEditor, docDock.VisibleDockables);
    }

    [Fact]
    public void ToggleWorldEditor_ReShow_AddsBack()
    {
        var vm = new MainWindowViewModel();
        var docDock = (Dock.Model.Mvvm.Controls.DocumentDock)vm.Layout!.VisibleDockables[0];
        var originalCount = docDock.VisibleDockables.Count;

        vm.IsWorldEditorVisible = false;
        vm.IsWorldEditorVisible = true;

        Assert.Equal(originalCount, docDock.VisibleDockables.Count);
        Assert.Contains(vm.WorldEditor, docDock.VisibleDockables);
    }

    [Fact]
    public void ToggleContentBrowser_HidesDockable()
    {
        var vm = new MainWindowViewModel();
        var docDock = (Dock.Model.Mvvm.Controls.DocumentDock)vm.Layout!.VisibleDockables[0];

        vm.IsContentBrowserVisible = false;

        Assert.DoesNotContain(vm.ContentBrowser, docDock.VisibleDockables);
    }

    [Fact]
    public void Toggle_IsIdempotent()
    {
        var vm = new MainWindowViewModel();
        var docDock = (Dock.Model.Mvvm.Controls.DocumentDock)vm.Layout!.VisibleDockables[0];
        var originalCount = docDock.VisibleDockables.Count;

        // Setting visible to its current value should not change anything
        vm.IsWorldEditorVisible = true;
        vm.IsContentBrowserVisible = true;
        Assert.Equal(originalCount, docDock.VisibleDockables.Count);

        // Setting visible to a value that matches state but with dockable
        // currently NOT in VisibleDockables (i.e. already hidden) should
        // not double-add
        vm.IsWorldEditorVisible = false;
        var countAfterHide = docDock.VisibleDockables.Count;
        vm.IsWorldEditorVisible = false; // already false
        Assert.Equal(countAfterHide, docDock.VisibleDockables.Count);
    }
}