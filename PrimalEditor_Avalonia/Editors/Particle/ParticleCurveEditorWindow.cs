using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Particle;

/// <summary>
/// B4 P3: 粒子曲线编辑器窗口（非模态、单例）。6 个槽位 tab
/// （Scale/Alpha/Color/Velocity/Force/Rotation），每个 tab = 启用开关 +
/// 插值模式（float/vector 槽位）+ <see cref="EmitterCurveEditor"/>。
/// 编辑即推引擎（EmitterSetCurveData / EmitterDisableCurve）。
/// 曲线数据仅存于运行中的发射器——场景文档不持久化曲线（B4 验收的持久化
/// 走预设文件链路，见计划 B4 P3 留痕）。
/// </summary>
public sealed class ParticleCurveEditorWindow : Window
{
    private static ParticleCurveEditorWindow? _instance;

    private uint _entityId;
    private readonly CheckBox[] _enables = new CheckBox[6];
    private readonly ComboBox[] _interpolations = new ComboBox[6];
    private readonly EmitterCurveEditor[] _editors = new EmitterCurveEditor[6];
    private bool _loading;

    public static void ShowFor(Avalonia.Controls.Window? owner, uint entityId)
    {
        if (_instance == null)
        {
            _instance = new ParticleCurveEditorWindow();
            _instance.Closed += (_, _) => _instance = null;
        }
        _instance.SetEntity(entityId);
        if (owner != null && _instance.Owner == null)
            _instance.Show(owner);
        else
            _instance.Show();
        _instance.Activate();
    }

    public ParticleCurveEditorWindow()
    {
        Title = "Particle Curves";
        Width = 760;
        Height = 480;
        CanResize = true;

        var tabs = new TabControl();
        var items = new Avalonia.Collections.AvaloniaList<object>();
        foreach (ParticleCurveSlot slot in Enum.GetValues<ParticleCurveSlot>())
        {
            int index = (int)slot;

            var editor = new EmitterCurveEditor { Slot = slot, MinHeight = 220 };
            editor.CurveEdited += () => PushSlot(index);
            _editors[index] = editor;

            var enable = new CheckBox
            {
                Content = "Enable curve",
                VerticalContentAlignment = VerticalAlignment.Center,
            };
            enable.IsCheckedChanged += (_, _) =>
            {
                if (_loading) return;
                OnEnableChanged(index, enable.IsChecked == true);
            };
            _enables[index] = enable;

            var interp = new ComboBox { MinWidth = 110, SelectedIndex = 1 };
            foreach (var name in new[] { "Constant", "Linear", "Smooth", "Bezier" })
                interp.Items.Add(name);
            interp.SelectionChanged += (_, _) =>
            {
                if (_loading || _editors[index].Curve == null) return;
                _editors[index].Curve!.Interpolation = (ParticleCurveInterpolation)interp.SelectedIndex;
                PushSlot(index);
            };
            _interpolations[index] = interp;

            var header = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 10,
                Children = { enable, interp },
            };

            var panel = new DockPanel { Margin = new Thickness(10), LastChildFill = true };
            DockPanel.SetDock(header, Avalonia.Controls.Dock.Top);
            panel.Children.Add(header);
            panel.Children.Add(editor);

            items.Add(new TabItem { Header = ParticleCurveData.SlotTitle(slot), Content = panel });
        }
        tabs.ItemsSource = items;
        Content = tabs;
    }

    /// <summary>切目标实体并从引擎拉全 6 槽位。</summary>
    public void SetEntity(uint entityId)
    {
        _entityId = entityId;
        Title = $"Particle Curves — entity {entityId:X}";
        for (int slot = 0; slot < 6; ++slot)
            LoadSlot(slot);
    }

    private void LoadSlot(int slot)
    {
        _loading = true;
        try
        {
            var data = ParticleCurveData.FromWire((ParticleCurveSlot)slot,
                NativeEngineBridge.EmitterGetCurveData(_entityId, (uint)slot));
            _editors[slot].Curve = data;
            _enables[slot].IsChecked = data != null;
            _interpolations[slot].IsEnabled = data != null;
            if (data != null)
                _interpolations[slot].SelectedIndex = (int)data.Interpolation;
            _editors[slot].InvalidateVisual();
        }
        finally
        {
            _loading = false;
        }
    }

    private void OnEnableChanged(int slot, bool enabled)
    {
        if (_entityId == 0) return;
        if (enabled)
        {
            // 无数据 → 建默认两点曲线（时间 0/1，值取槽位范围中点）
            if (_editors[slot].Curve == null)
            {
                var (min, max) = ParticleCurveData.SlotValueRange((ParticleCurveSlot)slot);
                float mid = (min + max) * 0.5f;
                var data = new ParticleCurveData { Slot = (ParticleCurveSlot)slot };
                data.Keys.Add(new ParticleCurveKey { Time = 0f, Value = new[] { mid } });
                data.Keys.Add(new ParticleCurveKey { Time = 1f, Value = new[] { mid } });
                if ((ParticleCurveSlot)slot == ParticleCurveSlot.ColorGradient)
                {
                    data.Keys[0].Value = new[] { 1f, 1f, 1f, 1f };
                    data.Keys[1].Value = new[] { 1f, 1f, 1f, 1f };
                }
                _editors[slot].Curve = data;
            }
            PushSlot(slot);
        }
        else
        {
            NativeEngineBridge.EmitterDisableCurve(_entityId, (uint)slot);
            _editors[slot].Curve = null;
        }
        _loading = true;
        _interpolations[slot].IsEnabled = _editors[slot].Curve != null;
        if (_editors[slot].Curve != null)
            _interpolations[slot].SelectedIndex = (int)_editors[slot].Curve!.Interpolation;
        _loading = false;
        _editors[slot].InvalidateVisual();
    }

    private void PushSlot(int slot)
    {
        if (_entityId == 0 || _editors[slot].Curve == null) return;
        var data = _editors[slot].Curve!;
        if (data.Keys.Count == 0) return;
        NativeEngineBridge.EmitterSetCurveData(_entityId, (uint)slot, data.ToWire());
    }
}
