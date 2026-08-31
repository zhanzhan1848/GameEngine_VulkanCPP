using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;

namespace PrimalEditor_Avalonia.Controls;

public enum VectorBoxDimension { Vec2, Vec3, Vec4 }

public class VectorBox : ContentControl
{
    private NumberBox? _xBox, _yBox, _zBox, _wBox;

    public static readonly StyledProperty<string> XProperty =
        AvaloniaProperty.Register<VectorBox, string>(nameof(X), defaultBindingMode: Avalonia.Data.BindingMode.TwoWay);
    public static readonly StyledProperty<string> YProperty =
        AvaloniaProperty.Register<VectorBox, string>(nameof(Y), defaultBindingMode: Avalonia.Data.BindingMode.TwoWay);
    public static readonly StyledProperty<string> ZProperty =
        AvaloniaProperty.Register<VectorBox, string>(nameof(Z), defaultBindingMode: Avalonia.Data.BindingMode.TwoWay);
    public static readonly StyledProperty<string> WProperty =
        AvaloniaProperty.Register<VectorBox, string>(nameof(W), defaultBindingMode: Avalonia.Data.BindingMode.TwoWay);
    public static readonly StyledProperty<VectorBoxDimension> DimensionProperty =
        AvaloniaProperty.Register<VectorBox, VectorBoxDimension>(nameof(Dimension), VectorBoxDimension.Vec3);
    public static readonly StyledProperty<Orientation> OrientationProperty =
        AvaloniaProperty.Register<VectorBox, Orientation>(nameof(Orientation), Orientation.Horizontal);
    public static readonly StyledProperty<double> MultiplierProperty =
        AvaloniaProperty.Register<VectorBox, double>(nameof(Multiplier), 0.01);

    public string X { get => GetValue(XProperty); set => SetValue(XProperty, value); }
    public string Y { get => GetValue(YProperty); set => SetValue(YProperty, value); }
    public string Z { get => GetValue(ZProperty); set => SetValue(ZProperty, value); }
    public string W { get => GetValue(WProperty); set => SetValue(WProperty, value); }
    public VectorBoxDimension Dimension { get => GetValue(DimensionProperty); set => SetValue(DimensionProperty, value); }
    public Orientation Orientation { get => GetValue(OrientationProperty); set => SetValue(OrientationProperty, value); }
    public double Multiplier { get => GetValue(MultiplierProperty); set => SetValue(MultiplierProperty, value); }

    static VectorBox()
    {
        XProperty.Changed.AddClassHandler<VectorBox>((b, e) => SyncBox(b._xBox, (string?)e.NewValue));
        YProperty.Changed.AddClassHandler<VectorBox>((b, e) => SyncBox(b._yBox, (string?)e.NewValue));
        ZProperty.Changed.AddClassHandler<VectorBox>((b, e) => SyncBox(b._zBox, (string?)e.NewValue));
        WProperty.Changed.AddClassHandler<VectorBox>((b, e) => SyncBox(b._wBox, (string?)e.NewValue));
        DimensionProperty.Changed.AddClassHandler<VectorBox>((b, _) => b.Rebuild());
        OrientationProperty.Changed.AddClassHandler<VectorBox>((b, _) => b.Rebuild());
    }

    public VectorBox()
    {
        Rebuild();
    }

    private void Rebuild()
    {
        Content = BuildContent();
        if (_xBox != null) _xBox.Value = X;
        if (_yBox != null) _yBox.Value = Y;
        if (_zBox != null) _zBox.Value = Z;
        if (_wBox != null) _wBox.Value = W;
    }

    private Panel BuildContent()
    {
        var orient = Orientation;
        var panel = new StackPanel { Orientation = orient, Spacing = 2 };

        _xBox = MakeBox();
        panel.Children.Add(_xBox);

        _yBox = MakeBox();
        _yBox.IsVisible = Dimension != VectorBoxDimension.Vec2;
        panel.Children.Add(_yBox);

        _zBox = MakeBox();
        _zBox.IsVisible = Dimension == VectorBoxDimension.Vec3 || Dimension == VectorBoxDimension.Vec4;
        panel.Children.Add(_zBox);

        _wBox = MakeBox();
        _wBox.IsVisible = Dimension == VectorBoxDimension.Vec4;
        panel.Children.Add(_wBox);

        return panel;
    }

    private NumberBox MakeBox()
    {
        var nb = new NumberBox { Multiplier = Multiplier, Width = 100 };
        return nb;
    }

    private static void SyncBox(NumberBox? box, string? value)
    {
        if (box != null && value != null && box.Value != value)
            box.Value = value;
    }
}