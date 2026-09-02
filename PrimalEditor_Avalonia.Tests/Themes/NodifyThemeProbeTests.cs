using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Media;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Themes;
using Xunit;

namespace PrimalEditor_Avalonia.Tests;

/// <summary>
/// 探针: 枚举 NodifyM.Avalonia 节点画布的主题画刷键在切换主题前后的取值,
/// 用于确认材质编辑器节点换肤的资源覆写机制。断言只验证"覆写生效",
/// 键不存在时跳过(兼容 NodifyM 版本演进)。
/// </summary>
[Collection("ThemeHeadlessSerial")]
public class NodifyThemeProbeTests
{
    private static readonly string[] CandidateKeys =
    {
        "NodeBrush", "NodeBorderBrush", "HeaderBrush", "HeaderForegroundBrush",
        "ContentBrush", "FooterBrush", "StrokeBrush", "TextBrush",
        "GridLinesColor", "SelectedBrush", "HighlightBrush",
        "SelectionRectangleBackgroundBrush", "SelectionRectangleColor",
        "SelectionRectangleStrokeBrush", "AlignmentLineBrush",
        "MapBorderBrush", "ViewportBrush", "ViewportBorderBrush", "BackgroundBrush",
    };

    [Fact]
    public async System.Threading.Tasks.Task NodifyBrushKeys_FollowActiveTheme()
    {
        var session = HeadlessUnitTestSession.StartNew(typeof(ThemeHeadlessTestApp));
        await session.Dispatch(() =>
        {
            var app = Application.Current!;
            // 启动主题 = Warcraft(默认): Nodify 键应为 Warcraft 覆写值
            var found = new System.Collections.Generic.List<string>();
            var warcraftValues = new System.Collections.Generic.Dictionary<string, object>();
            foreach (var key in CandidateKeys)
            {
                try
                {
                    var v = app.FindResource(key);
                    if (v != null) { found.Add(key); warcraftValues[key] = v; }
                }
                catch { /* 键不存在 */ }
            }

            Assert.True(found.Count > 0,
                "no NodifyM theme keys resolved; key names changed in NodifyM.Avalonia");

            // 切到 Cyberpunk: 该主题不定义 Nodify 键 → 回落 NodifyM 默认值
            Assert.True(ThemeManager.Apply(EditorThemeRegistry.CyberpunkId));
            Dispatcher.UIThread.RunJobs();

            var changed = new System.Collections.Generic.List<string>();
            foreach (var key in found)
            {
                // 覆写移除后键可能解析为 null(默认值改由 NodifyM 的样式作用域
                // 资源提供给控件, 应用级查不到)—— null 也算"已变化"
                object? v = null;
                try { v = app.FindResource(key); } catch { }
                if (!Equals(warcraftValues[key], v)) changed.Add(key);
            }

            Assert.True(changed.Count > 0,
                $"current={ThemeManager.Current.Id}; found=[{string.Join(',', found)}] changed=[{string.Join(',', changed)}] " +
                $"sample NodeBrush before={(warcraftValues.TryGetValue("NodeBrush", out var nb) ? nb : null)} after={(app.FindResource("NodeBrush") as IBrush)}");

            // 切回默认, 保持进程内主题状态干净
            ThemeManager.Apply(EditorThemeRegistry.WarcraftId);
            Dispatcher.UIThread.RunJobs();
        }, System.Threading.CancellationToken.None);
    }
}
