using Avalonia.Controls;

namespace PrimalEditor_Avalonia.Views;

/// <summary>
/// Sequencer 标签页的宿主视图（ViewLocator 约定：Dockable.Id "Sequencer"
/// → Views.SequencerView）。只包一层 <c>SequencerPanel</c>，面板自身逻辑
/// 不变——它经 <c>EditorServices.FindViewport()</c> 定位视口，与所在的
/// 标签页解耦。
/// </summary>
public partial class SequencerView : UserControl
{
    public SequencerView()
    {
        InitializeComponent();

    }
}
