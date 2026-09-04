using System;
using Avalonia.Controls;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

/// <summary>
/// A8 源码控制最小集成：非模态面板（git 状态列表 + 提交）。
/// 单例窗口——重复点工具栏按钮只激活已有实例。
/// </summary>
public partial class SourceControlPanel : Window
{
    private static SourceControlPanel? _instance;

    public static void ShowFor(Avalonia.Controls.Window? owner, string? projectPath)
    {
        if (_instance == null)
        {
            _instance = new SourceControlPanel();
            _instance.Closed += (_, _) => _instance = null;
        }
        if (_instance.DataContext is SourceControlViewModel vm)
            vm.SetProject(projectPath);
        if (owner != null && _instance.Owner == null)
            _instance.Show(owner);
        else
            _instance.Show();
        _instance.Activate();
    }

    public SourceControlPanel()
    {
        InitializeComponent();
        DataContext = new SourceControlViewModel();
    }
}
