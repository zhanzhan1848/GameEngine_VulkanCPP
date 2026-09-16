using Dock.Controls.DeferredContentControl;
using Dock.Model.Mvvm.Controls;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>
/// Sequencer 标签页的 Dockable ViewModel（§View 菜单开关标签页）。与
/// WorldEditorViewModel / ContentBrowserViewModel 同一模式：仅承担
/// Document 身份（Id/Title 由 <see cref="Common.MainWindowDockFactory"/>
/// 填写），真实 UI 逻辑在 <c>Views.Sequencer.SequencerPanel</c> 的
/// code-behind——ViewLocator 按 Id "Sequencer" 约定解析到
/// <c>Views.SequencerView</c> 包装视图。
/// DeferContentPresentation=false：Dock 12.1 的延迟呈现（reveal 时间线）
/// 在真实窗口环境下不刷新内容区（实测标签空白），声明同步呈现。
/// </summary>
public class SequencerViewModel : Document, IDeferredContentPresentation
{
    public bool DeferContentPresentation => false;
}
