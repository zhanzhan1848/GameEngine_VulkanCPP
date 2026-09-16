using System.Linq;
using Dock.Model.Core;
using Dock.Model.Mvvm.Controls;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

/// <summary>Sequencer is a local tool tab; F7 must not mutate the document dock.</summary>
public sealed class SequencerTabTests
{
    private static DocumentDock? FindDocumentDock(IDockable? node)
    {
        if (node is DocumentDock docs) return docs;
        if (node is IDock dock && dock.VisibleDockables != null)
        {
            foreach (var child in dock.VisibleDockables)
            {
                var found = FindDocumentDock(child);
                if (found != null) return found;
            }
        }
        return null;
    }

    [Fact]
    public void ToggleSequencerTab_SelectsBottomPanelWithoutChangingDocuments()
    {
        var vm = new MainWindowViewModel();
        var docs = FindDocumentDock(vm.Layout)!;
        var documents = docs.VisibleDockables!.ToArray();
        Assert.False(vm.IsSequencerVisible);
        vm.ToggleSequencerTab();
        Assert.True(vm.IsSequencerVisible);
        Assert.Equal(1, vm.WorldEditor.BottomPanelIndex);
        Assert.Equal(documents, docs.VisibleDockables.ToArray());
        Assert.Same(vm.WorldEditor, docs.ActiveDockable);
        Assert.Null(vm.Sequencer.Owner);
        vm.ToggleSequencerTab();
        Assert.False(vm.IsSequencerVisible);
        Assert.Equal(0, vm.WorldEditor.BottomPanelIndex);
        Assert.Equal(documents, docs.VisibleDockables.ToArray());
        // Clicking the local tab also updates the menu's state.
        vm.WorldEditor.BottomPanelIndex = 1;
        Assert.True(vm.IsSequencerVisible);
    }
}
