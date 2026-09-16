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

        // Sequencer 只登记 Id/Title（ViewLocator 按 Id 解析视图），
        // 不放进初始 VisibleDockables；若被 ShowDockable 显示则由
        // DocumentDock 兜底加回标签组（F7 本体切底部面板，见
        // SequencerTabTests）。
        var sequencer = _vm.Sequencer;
        sequencer.Id = "Sequencer";
        sequencer.Title = "Sequencer";

        // VisibleDockables 必须用 Factory.CreateList（可通知集合）：
        // 运行期经 View 菜单增删标签依赖 CollectionChanged 刷新视图，
        // 普通 List 的增删不会让标签条重绘（F7 标签不出现的实测根因）。
        var documents = new DocumentDock
        {
            Id = "MainDocuments",
            Title = "Documents",
            VisibleDockables = CreateList(new IDockable[] { worldEditor }),
            ActiveDockable = worldEditor,
        };

        var root = CreateRootDock();
        root.Id = "MainRoot";
        root.Title = "Root";
        root.VisibleDockables = CreateList(new IDockable[] { documents });
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