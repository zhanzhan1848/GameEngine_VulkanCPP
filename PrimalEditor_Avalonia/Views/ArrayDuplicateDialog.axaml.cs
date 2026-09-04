using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Views;

/// <summary>
/// A5 Array 批量布设参数对话框：轴 + 数量 + 间距。
/// ShowDialog&lt;bool?&gt; 后读 Axis/Count/Offset（Confirmed == true 时有效）。
/// </summary>
public partial class ArrayDuplicateDialog : Window
{
    public bool Confirmed { get; private set; }
    public string Axis => AxisCombo.SelectedIndex switch
    {
        1 => "Y",
        2 => "Z",
        _ => "X",
    };
    public int Count { get; private set; } = 4;
    public float Offset { get; private set; } = 1f;

    public ArrayDuplicateDialog()
    {
        InitializeComponent();
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private void OnOk(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (!int.TryParse(CountBox.Text, out var count) || count < 1 || count > 256)
        {
            CountBox.Text = "4";
            return;
        }
        if (!float.TryParse(OffsetBox.Text,
                System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out var offset) || offset == 0f)
        {
            OffsetBox.Text = "1";
            return;
        }
        Count = count;
        Offset = offset;
        Confirmed = true;
        Close();
    }

    private void OnCancel(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        Confirmed = false;
        Close();
    }
}
