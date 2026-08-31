using Dock.Model.Core;
using Dock.Model.Mvvm.Controls;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;
using Xunit.Abstractions;

namespace PrimalEditor_Avalonia.Tests.Docking;

public sealed class MainWindowDockToggleTests
{
    private readonly ITestOutputHelper _out;
    public MainWindowDockToggleTests(ITestOutputHelper output) { _out = output; }

    [Fact]
    public void ToggleWorldEditor_HidesDockable()
    {
        var vm = new MainWindowViewModel();
        var layout = vm.Layout!;
        _out.WriteLine($"Layout runtime type: {layout.GetType().FullName}");
        _out.WriteLine($"Layout is IDock: {layout is IDock}");
        var docDock = FindDocumentDock(layout);
        Assert.NotNull(docDock);
        Assert.Contains(vm.WorldEditor, docDock!.VisibleDockables);

        vm.IsWorldEditorVisible = false;

        _out.WriteLine($"After toggle, WorldEditor still in docDock: {docDock.VisibleDockables.Contains(vm.WorldEditor)}");
        var docDockAfter = FindDocumentDock(layout);
        Assert.NotNull(docDockAfter);
        Assert.DoesNotContain(vm.WorldEditor, docDockAfter!.VisibleDockables);
    }

    [Fact]
    public void ToggleWorldEditor_ReShow_AddsBack()
    {
        var vm = new MainWindowViewModel();
        var docDock = FindDocumentDock(vm.Layout!);
        Assert.NotNull(docDock);
        var originalCount = docDock!.VisibleDockables.Count;

        vm.IsWorldEditorVisible = false;
        vm.IsWorldEditorVisible = true;

        Assert.Equal(originalCount, docDock.VisibleDockables.Count);
        Assert.Contains(vm.WorldEditor, docDock.VisibleDockables);
    }

    [Fact]
    public void Toggle_IsIdempotent()
    {
        var vm = new MainWindowViewModel();
        var docDock = FindDocumentDock(vm.Layout!);
        var originalCount = docDock!.VisibleDockables.Count;

        vm.IsWorldEditorVisible = true;
        Assert.Equal(originalCount, docDock.VisibleDockables.Count);

        vm.IsWorldEditorVisible = false;
        var countAfterHide = docDock.VisibleDockables.Count;
        vm.IsWorldEditorVisible = false;
        Assert.Equal(countAfterHide, docDock.VisibleDockables.Count);
    }

    private static DocumentDock? FindDocumentDock(IDockable root)
    {
        if (root is IDock rootDock)
        {
            foreach (var child in rootDock.VisibleDockables)
            {
                if (child is DocumentDock docs) return docs;
                if (child is IDock innerDock)
                {
                    foreach (var grand in innerDock.VisibleDockables)
                        if (grand is DocumentDock d2) return d2;
                }
            }
        }
        return null;
    }

    private static ToolDock? FindToolDock(IDockable root)
    {
        if (root is IDock rootDock)
        {
            foreach (var child in rootDock.VisibleDockables)
            {
                if (child is ToolDock tool) return tool;
                if (child is IDock innerDock)
                {
                    foreach (var grand in innerDock.VisibleDockables)
                        if (grand is ToolDock t2) return t2;
                }
            }
        }
        return null;
    }
}