using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Data;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;

namespace PrimalEditor_Avalonia.Controls;

public enum NumberBoxType { Float, Int, UInt }

public class NumberBox : TemplatedControl
{
    private TextBlock? _label;
    private TextBox?   _editor;
    private double     _original;
    private double     _mouseStart;
    private bool       _captured;
    private bool       _changedSincePress;

    public static readonly StyledProperty<string> ValueProperty =
        AvaloniaProperty.Register<NumberBox, string>(nameof(Value),
            defaultBindingMode: BindingMode.TwoWay);

    public static readonly StyledProperty<double> MultiplierProperty =
        AvaloniaProperty.Register<NumberBox, double>(nameof(Multiplier), 0.01);

    public static readonly StyledProperty<NumberBoxType> KindProperty =
        AvaloniaProperty.Register<NumberBox, NumberBoxType>(nameof(Kind));

    public static readonly StyledProperty<bool> ReadOnlyProperty =
        AvaloniaProperty.Register<NumberBox, bool>(nameof(ReadOnly));

    public string Value       { get => GetValue(ValueProperty);       set => SetValue(ValueProperty, value); }
    public double Multiplier  { get => GetValue(MultiplierProperty);  set => SetValue(MultiplierProperty, value); }
    public NumberBoxType Kind { get => GetValue(KindProperty);        set => SetValue(KindProperty, value); }
    public bool ReadOnly      { get => GetValue(ReadOnlyProperty);    set => SetValue(ReadOnlyProperty, value); }

    public event EventHandler<string>? ValueCommitted;
    public event EventHandler<string>? ValueDragging;

    static NumberBox()
    {
        ValueProperty.Changed.AddClassHandler<NumberBox>((b, _) => b.SyncTemplate());
    }

    protected override void OnApplyTemplate(TemplateAppliedEventArgs e)
    {
        base.OnApplyTemplate(e);
        _label  = e.NameScope.Find<TextBlock>("PART_TextBlock");
        _editor = e.NameScope.Find<TextBox>("PART_TextBox");

        if (_label != null)
        {
            _label.PointerPressed  += OnLabelPointerPressed;
            _label.PointerMoved    += OnLabelPointerMoved;
            _label.PointerReleased += OnLabelPointerReleased;
            _label.DoubleTapped    += OnLabelDoubleTapped;
        }
        if (_editor != null)
        {
            _editor.KeyDown   += OnEditorKeyDown;
            _editor.LostFocus += OnEditorLostFocus;
        }
        SyncTemplate();
    }

    private void SyncTemplate()
    {
        var s = Value ?? "";
        if (_label != null)  _label.Text  = s;
        if (_editor != null && _editor.Text != s) _editor.Text = s;
    }

    private void OnLabelPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (ReadOnly || _label == null) return;
        double.TryParse(Value, System.Globalization.NumberStyles.Any,
            System.Globalization.CultureInfo.InvariantCulture, out _original);
        e.Pointer.Capture(_label);
        _captured = true;
        _changedSincePress = false;
        _mouseStart = e.GetPosition(_label).X;
        e.Handled = true;
    }

    private void OnLabelPointerMoved(object? sender, PointerEventArgs e)
    {
        if (!_captured || _label == null) return;
        var x = e.GetPosition(_label).X;
        var delta = (x - _mouseStart) * Multiplier;
        var next  = _original + delta;
        next = Kind switch
        {
            NumberBoxType.Int   => System.Math.Round(next),
            NumberBoxType.UInt  => System.Math.Max(0, System.Math.Round(next)),
            _                   => next,
        };
        var s = next.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
        if (s != Value)
        {
            Value = s;
            ValueDragging?.Invoke(this, s);
            _changedSincePress = true;
        }
        e.Handled = true;
    }

    private void OnLabelPointerReleased(object? sender, PointerReleasedEventArgs e)
    {
        if (!_captured) return;
        e.Pointer.Capture(null);
        _captured = false;
        if (_changedSincePress)
            ValueCommitted?.Invoke(this, Value);
        e.Handled = true;
    }

    private void OnLabelDoubleTapped(object? sender, Avalonia.Input.TappedEventArgs e)
    {
        if (ReadOnly || _editor == null) return;
        _editor.IsVisible = true;
        _label!.IsVisible = false;
        Dispatcher.UIThread.Post(() => { _editor.Focus(); _editor.SelectAll(); });
        e.Handled = true;
    }

    private void OnEditorKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && _editor != null)
        {
            Commit(_editor.Text);
            _editor.IsVisible = false;
            _label!.IsVisible = true;
            e.Handled = true;
        }
        else if (e.Key == Key.Escape && _editor != null)
        {
            _editor.Text = Value;
            _editor.IsVisible = false;
            _label!.IsVisible = true;
            e.Handled = true;
        }
    }

    private void OnEditorLostFocus(object? sender, RoutedEventArgs e)
    {
        if (_editor == null) return;
        Commit(_editor.Text);
        _editor.IsVisible = false;
        _label!.IsVisible = true;
    }

    private void Commit(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            Value = "0";
            ValueCommitted?.Invoke(this, Value);
            return;
        }
        if (double.TryParse(text, System.Globalization.NumberStyles.Any,
            System.Globalization.CultureInfo.InvariantCulture, out var d))
        {
            d = Kind switch
            {
                NumberBoxType.Int   => System.Math.Round(d),
                NumberBoxType.UInt  => System.Math.Max(0, System.Math.Round(d)),
                _                   => d,
            };
            Value = d.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
            ValueCommitted?.Invoke(this, Value);
        }
        else
        {
            Value = _original.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
        }
    }
}