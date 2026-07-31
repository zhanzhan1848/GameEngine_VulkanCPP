using System.Text.Json;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Docking;

/// <summary>
/// Diagnostic test for the user-reported "no tabs" issue. Reads the
/// actual `editor-layout.json` (if present) and traces what
/// `ApplyLayout` would produce.
/// </summary>
public sealed class ApplyLayoutDiagnosticTests
{
    [Fact]
    public void SavedLayout_ProducesDocumentDockWithBothEntries()
    {
        const string json = @"{
            ""Entries"": [
                { ""Id"": ""WorldEditor"", ""Zone"": 4, ""IsFloating"": false, ""X"": 0, ""Y"": 0, ""Width"": 600, ""Height"": 400, ""TabIndex"": 0 },
                { ""Id"": ""ContentBrowser"", ""Zone"": 4, ""IsFloating"": false, ""X"": 0, ""Y"": 0, ""Width"": 600, ""Height"": 400, ""TabIndex"": 1 }
            ],
            ""ActiveDockableId"": null,
            ""SchemaVersion"": 1
        }";

        var layout = JsonSerializer.Deserialize<DockingLayout>(json);
        Assert.NotNull(layout);
        Assert.Equal(2, layout.Entries.Count);

        var vm = new MainWindowViewModel();
        var factory = new MainWindowDockFactory(vm);
        var tree = DockingLayoutBridge.ToDockTree(layout, factory, entry => entry.Id switch
        {
            "WorldEditor" => vm.WorldEditor,
            "ContentBrowser" => vm.ContentBrowser,
            _ => null,
        });

        Assert.Single(tree.VisibleDockables); // 1 DocumentDock
        var doc = Assert.IsType<Dock.Model.Mvvm.Controls.DocumentDock>(tree.VisibleDockables[0]);
        Assert.Equal(2, doc.VisibleDockables.Count);
        Assert.Contains(vm.WorldEditor, doc.VisibleDockables);
        Assert.Contains(vm.ContentBrowser, doc.VisibleDockables);
    }
}