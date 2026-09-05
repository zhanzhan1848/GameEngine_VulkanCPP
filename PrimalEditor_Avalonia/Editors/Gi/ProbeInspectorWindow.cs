using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Media;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Gi;

/// <summary>
/// B1 L2: DDGI probe inspector（非模态单例）。经共享内存直读 SH9 irradiance
/// 与 confidence 缓冲（Metal Dynamic 缓冲可 Map，无需帧内 stage-copy）。
/// 每个 probe 显示 confidence（convergenceAge 条）与 6 方向（±X±Y±Z）SH9
/// 评估色板。SH 数学逐行镜像 CommonFunction.metal::EvalSH9Irradiance。
/// </summary>
public sealed class ProbeInspectorWindow : Window
{
    private static ProbeInspectorWindow? _instance;

    private readonly StackPanel _rows = new();
    private readonly TextBlock _status = new();

    public static void ShowFor(Avalonia.Controls.Window? owner)
    {
        if (_instance == null)
        {
            _instance = new ProbeInspectorWindow();
            _instance.Closed += (_, _) => _instance = null;
        }
        if (owner != null && _instance.Owner == null)
            _instance.Show(owner);
        else
            _instance.Show();
        _instance.Activate();
        _instance.Refresh();
    }

    public ProbeInspectorWindow()
    {
        Title = "DDGI Probe Inspector";
        Width = 720;
        Height = 560;

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto"), Margin = new Thickness(10) };

        var top = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var refresh = new Button { Content = "Refresh" };
        refresh.Click += (_, _) => Refresh();
        top.Children.Add(refresh);
        top.Children.Add(_status);
        root.Children.Add(top);

        var scroll = new ScrollViewer { Margin = new Thickness(0, 8, 0, 0) };
        scroll.Content = _rows;
        root.Children.Add(scroll);

        Content = root;
    }

    public void Refresh()
    {
        _rows.Children.Clear();
        var meta = NativeEngineBridge.GIGetProbeDebugMeta();
        if (meta == null)
        {
            _status.Text = "DDGI not initialized (toggle a GI render mode first)";
            return;
        }

        var confidence = NativeEngineBridge.GIProbeConfidenceReadback((int)meta.Total);
        var irradiance = NativeEngineBridge.GIProbeIrradianceReadback((int)meta.Total);
        _status.Text = $"{meta.CountX}×{meta.CountY}×{meta.CountZ} = {meta.Total} probes, " +
                       $"spacing {meta.Spacing:0.##}m, frame slot {meta.FrameSlot}";

        int shown = 0;
        for (uint p = 0; p < meta.Total && shown < 512; ++p, ++shown)
        {
            float conv = confidence.Length >= p * 4 + 4 ? confidence[p * 4 + 3] : 0f;
            float vis = confidence.Length >= p * 4 + 3 ? confidence[p * 4 + 2] : 0f;

            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };

            row.Children.Add(new TextBlock
            {
                Text = $"#{p}",
                Width = 46,
                FontFamily = new FontFamily("Menlo,Consolas,monospace"),
                FontSize = 11,
                VerticalAlignment = VerticalAlignment.Center,
            });

            var confBar = new Border
            {
                Width = 80,
                Height = 10,
                CornerRadius = new CornerRadius(2),
                Background = new SolidColorBrush(ConfidenceBrush(conv)),
                VerticalAlignment = VerticalAlignment.Center,
            };
            ToolTip.SetTip(confBar, $"convAge={conv:0.00} visConf={vis:0.00}");
            row.Children.Add(confBar);

            if (irradiance.Length >= (p + 1) * 9 * 4)
            {
                foreach (var dir in new[]
                {
                    (1f, 0f, 0f), (-1f, 0f, 0f),
                    (0f, 1f, 0f), (0f, -1f, 0f),
                    (0f, 0f, 1f), (0f, 0f, -1f),
                })
                {
                    var c = EvalSH9(irradiance, (int)(p * 9 * 4), dir.Item1, dir.Item2, dir.Item3);
                    var swatch = new Border
                    {
                        Width = 16,
                        Height = 16,
                        CornerRadius = new CornerRadius(2),
                        Background = new SolidColorBrush(c),
                    };
                    ToolTip.SetTip(swatch,
                        $"dir ({dir.Item1:+0;-0},{dir.Item2:+0;-0},{dir.Item3:+0;-0})");
                    row.Children.Add(swatch);
                }
            }

            _rows.Children.Add(row);
        }

        if (shown == 0)
            _rows.Children.Add(new TextBlock { Text = "(no probes)" });
    }

    /// <summary>SH9 → RGB，逐行镜像 CommonFunction.metal::EvalSH9Irradiance。</summary>
    private static Color EvalSH9(float[] irr, int baseFloat, float nx, float ny, float nz)
    {
        const float A0 = 3.14159265f, A1 = 2.09439510f, A2 = 0.78539816f;
        const float C0 = 0.28209479f, C1 = 0.48860251f;
        const float C2_0 = 1.09254843f, C2_1 = 0.31539156f, C2_2 = 0.54627421f;

        float r = 0, g = 0, b = 0;
        void Add(int i, float w)
        {
            r += irr[baseFloat + i * 4 + 0] * w;
            g += irr[baseFloat + i * 4 + 1] * w;
            b += irr[baseFloat + i * 4 + 2] * w;
        }

        Add(0, A0 * C0);
        Add(1, A1 * -C1 * ny);
        Add(2, A1 * C1 * nz);
        Add(3, A1 * -C1 * nx);
        Add(4, A2 * C2_0 * nx * ny);
        Add(5, A2 * -C2_0 * ny * nz);
        Add(6, A2 * C2_1 * nz);
        Add(7, A2 * -C2_1 * nx);
        Add(8, A2 * C2_2 * (3f * nz * nz - 1f));

        float tone(float v) => v / (1f + v);
        byte sq(float v) => (byte)Math.Clamp((int)(MathF.Sqrt(tone(v)) * 255f), 0, 255);
        return Color.FromRgb(sq(r), sq(g), sq(b));
    }

    private static Color ConfidenceBrush(float convergenceAge) => convergenceAge switch
    {
        < 0.25f => Color.FromRgb(120, 220, 120),
        < 0.6f => Color.FromRgb(230, 200, 90),
        _ => Color.FromRgb(230, 120, 90),
    };
}
