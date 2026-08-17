using System;
using System.Collections.Generic;
using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Builds the <see cref="IRootDock"/> tree for MainWindow. The root
/// hosts a single <see cref="DocumentDock"/> with World Editor as the
/// only Document — the left panel (Hierarchy / Content Browser) and the
/// bottom panel (Build Log / Logger) live inside World Editor's own
/// XAML, not as separate Dockables.
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
        var worldEditor = _vm.WorldEditor;
        worldEditor.Id = "WorldEditor";
        worldEditor.Title = "World Editor";

        var documents = new DocumentDock
        {
            Id = "MainDocuments",
            Title = "Documents",
            VisibleDockables = new List<IDockable> { worldEditor },
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
        DockableLocator = new Dictionary<string, Func<IDockable?>>();
        ContextLocator = new Dictionary<string, Func<object?>>();
        base.InitLayout(layout);
    }
}