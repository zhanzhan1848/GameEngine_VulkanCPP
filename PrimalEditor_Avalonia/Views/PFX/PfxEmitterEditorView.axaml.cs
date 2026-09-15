using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Markup.Xaml;
using System;
using Avalonia.VisualTree;
using NodifyM.Avalonia.Controls;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Editors.Particle;
using PrimalEditor_Avalonia.ViewModels.PFX;

namespace PrimalEditor_Avalonia.Views.PFX;

public partial class PfxEmitterEditorView : UserControl
{
    public PfxEmitterEditorView()
    {
        InitializeComponent();
        // Wire the embedded curve editor's drag-end + the resident preview
        // loop. CurveEdited is a plain Action (shared with the legacy curve
        // window), so subscribe in code rather than via an XAML event.

        DataContextChanged += (_, _) => AttachVm();

        // NodifyM 的 Node 会为拖拽处理 PointerPressed（标记 Handled），
        // 模板级订阅收不到 —— 用 handledEventsToo 在编辑器层兜底捕获节点点击。
        var editor = this.FindControl<NodifyM.Avalonia.Controls.NodifyEditor>("PipelineEditor");
        // 隧道 + 冒泡双注册：无论 NodifyM 内部是否标记 Handled，选择都会先于/晚于
        // 其处理各触发一次（SelectNode 幂等，重复调用无害）。
        editor?.AddHandler(Avalonia.Input.InputElement.PointerPressedEvent, OnEditorPointerPressed,
            Avalonia.Interactivity.RoutingStrategies.Tunnel, handledEventsToo: true);
        editor?.AddHandler(Avalonia.Input.InputElement.PointerPressedEvent, OnEditorPointerPressed,
            Avalonia.Interactivity.RoutingStrategies.Bubble, handledEventsToo: true);
        editor?.AddHandler(Avalonia.Input.InputElement.KeyDownEvent, OnEditorKeyDown,
            Avalonia.Interactivity.RoutingStrategies.Tunnel);
    }

    /// <summary>嵌在参数行里的曲线编辑器（每个曲线参数一个）编辑完成回调。</summary>
    private void OnCurveEdited(object? sender, EventArgs e)
    {
        if (DataContext is PfxEmitterEditorViewModel vm) vm.NotifyCurveEdited();
    }

    private void OnRemoveModuleClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not PfxEmitterEditorViewModel vm) return;
        if (e.Source is Avalonia.Controls.Button b && b.CommandParameter is PfxStackModuleRow row)
            vm.RemoveModuleRow(row);
    }

    private void OnEditorKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key is not (Key.Delete or Key.Back)) return;
        if (e.Source is TextBox) return;   // 文本编辑中的删除不抢
        if (DataContext is not PfxEmitterEditorViewModel vm) return;
        // 删除当前选中（画布点击/下拉选择）的模块；无选中则忽略
        if (vm.SelectedModule is { } selected)
        {
            vm.RemoveModuleRow(selected);
            e.Handled = true;
        }
    }

    private void OnEditorPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (DataContext is not PfxEmitterEditorViewModel vm) return;
        // 沿可视树向上找节点 VM（点击可能落在 Node 的任意子元素上）
        Avalonia.Visual? v = e.Source as Avalonia.Visual;
        while (v != null)
        {
            if (v.DataContext is PfxNodeViewModel node)
            {
                vm.SelectNode(node);
                return;
            }
            v = v.GetVisualParent();
        }
    }

    private void AttachVm()
    {
        if (DataContext is PfxEmitterEditorViewModel vm)
        {
            vm.AttachUndoCoalescer();
            vm.StartLivePreviewLoop();
        }
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnSaveClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is PfxEmitterEditorViewModel vm) vm.Save();
    }

    private void OnDiscardClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is PfxEmitterEditorViewModel vm) vm.Discard();
    }

    /// <summary>Canvas node click → selects the module (terminals → root settings).</summary>
    private void OnNodePointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (DataContext is not PfxEmitterEditorViewModel vm) return;
        if (sender is Node { DataContext: PfxNodeViewModel node })
            vm.SelectNode(node);
    }

    /// <summary>Double-click a palette module to append an instance to the stack.</summary>
    private void OnPaletteDoubleTapped(object? sender, TappedEventArgs e)
    {
        if (DataContext is not PfxEmitterEditorViewModel vm) return;
        if (sender is ListBox list && list.SelectedItem is PaletteModuleViewModel palette)
            vm.AddModuleFromPalette(palette);
    }
}
