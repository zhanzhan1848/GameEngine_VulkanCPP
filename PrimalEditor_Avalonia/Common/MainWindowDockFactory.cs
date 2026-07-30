using System;
using System.Collections.Generic;
using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Builds the <see cref="IRootDock"/> tree for MainWindow. The
/// <see cref="MainWindowViewModel.WorldEditor"/> and
/// <see cref="MainWindowViewModel.ContentBrowser"/> view models
/// are <see cref="Dock.Model.Mvvm.Controls.Document"/> subclasses
/// (added in P1.d Step 5), so they can be added directly to a
/// <see cref="DocumentDock"/>. <see cref="MainWindowViewModel.Logger"/>
/// is intentionally NOT a Dockable — it stays in its fixed bottom
/// row of the MainWindow Grid.
/// </summary>
public sealed class MainWindowDockFactory : Factory
{
    private readonly MainWindowViewModel _vm;

    public MainWindowDockFactory(MainWindowViewModel vm)
    {
        _vm = vm;
    }

    public override IRootDock CreateLayout()
    {
        // The two view models are now Dockables (Document).
        // Logger is NOT a Dockable — it stays in its fixed bottom
        // row of MainWindow's Grid. Only the 2 main views become
        // tabs; Logger remains a separate fixed panel.
        var worldEditor = _vm.WorldEditor;   // WorldEditorViewModel : Document
        var contentBrowser = _vm.ContentBrowser;  // ContentBrowserViewModel : Document

        worldEditor.Id = "WorldEditor";
        worldEditor.Title = "World Editor";
        contentBrowser.Id = "ContentBrowser";
        contentBrowser.Title = "Content Browser";

        var documents = new DocumentDock
        {
            Id = "MainDocuments",
            Title = "Documents",
            VisibleDockables = new List<IDockable> { worldEditor, contentBrowser },
            ActiveDockable = worldEditor,
        };

        var root = CreateRootDock();
        root.Id = "MainRoot";
        root.Title = "Root";
        root.VisibleDockables = new List<IDockable> { documents };
        root.ActiveDockable = documents;
        return root;
    }

    public override void InitLayout(IDockable layout)
    {
        // DockableLocator: nothing custom needed for v1 — the
        // factory's CreateLayout already wired the Dockables
        // directly. If we later want to look up Dockables by id
        // (e.g. for serialization round-trip), add entries here.
        DockableLocator = new Dictionary<string, Func<IDockable?>>();

        // ContextLocator: each Dockable already has its own VM as
        // its Context (the ViewModel IS the Dockable in our design).
        // We don't need to provide a separate model — the
        // existing ViewLocator reads DataContext = the Dockable
        // itself.
        ContextLocator = new Dictionary<string, Func<object?>>();

        base.InitLayout(layout);
    }
}