using System;
using Avalonia;
using Avalonia.Controls;

namespace PrimalEditor_Avalonia.Themes.Controls;

/// <summary>
/// 附加属性: 为任意控件声明九宫格外框参数。消费方是控件模板
/// (如 Warcraft 主题的 NodifyM Node 模板里的 NineSliceImage, 经
/// TemplateBinding 读取), 因此框随控件自身的视觉变换/裁剪走,
/// 不会逃出画布。其它主题不设置 Source → 模板里的框图为空。
/// </summary>
public class Frame
{
    // 静态类不能作为 RegisterAttached 的 owner 类型参数,保留私有构造即可
    private Frame() { }

    public static readonly AttachedProperty<string?> SourceProperty =
        AvaloniaProperty.RegisterAttached<Frame, Control, string?>("Source");

    public static readonly AttachedProperty<double> SliceProperty =
        AvaloniaProperty.RegisterAttached<Frame, Control, double>("Slice", 8.0);

    public static readonly AttachedProperty<double> ScaleProperty =
        AvaloniaProperty.RegisterAttached<Frame, Control, double>("Scale", 2.0);

    /// <summary>节点内容向内收缩量(DIP), 给框环让位; 0 = 不收缩。</summary>
    public static readonly AttachedProperty<double> InsetProperty =
        AvaloniaProperty.RegisterAttached<Frame, Control, double>("Inset");

    public static string? GetSource(Control control) => control.GetValue(SourceProperty);
    public static void SetSource(Control control, string? value) => control.SetValue(SourceProperty, value);
    public static double GetSlice(Control control) => control.GetValue(SliceProperty);
    public static void SetSlice(Control control, double value) => control.SetValue(SliceProperty, value);
    public static double GetScale(Control control) => control.GetValue(ScaleProperty);
    public static void SetScale(Control control, double value) => control.SetValue(ScaleProperty, value);
    public static double GetInset(Control control) => control.GetValue(InsetProperty);
    public static void SetInset(Control control, double value) => control.SetValue(InsetProperty, value);
}
