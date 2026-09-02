using System;
using System.IO;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Headless;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Layout;
using Avalonia.Skia;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Themes;
using Xunit;

namespace PrimalEditor_Avalonia.Tests;

/// <summary>
/// 非提交用的视觉检查工具:在 headless 会话里套用 Liquid Glass 主题,
/// 渲染一组典型编辑器控件(玻璃卡片/按钮/输入/列表/进度条/折叠卡)
/// 输出 PNG 到 /tmp,人工核对玻璃质感(半透明填充/高光边缘/彩色投影)
/// 与浅色底板的可读性。运行:
/// dotnet test --filter FullyQualifiedName~LiquidGlassVisualDump
/// </summary>
[Collection("ThemeHeadlessSerial")]
public class LiquidGlassVisualDump
{
    [Fact]
    public async System.Threading.Tasks.Task Dump_LiquidGlassControls()
    {
        var session = HeadlessUnitTestSession.StartNew(typeof(ThemeHeadlessTestApp));
        await session.Dispatch(() =>
        {
            ThemeManager.Apply(EditorThemeRegistry.LiquidGlassId);
            Dispatcher.UIThread.RunJobs();

            try
            {
                DumpCollage();
            }
            finally
            {
                // 还原默认主题,避免静态 ThemeManager 状态泄漏到同进程的其它测试
                ThemeManager.Apply(EditorThemeRegistry.CyberpunkId);
            }
        }, System.Threading.CancellationToken.None);
    }

    private static void DumpCollage()
    {
        var floatingOnly = new Border
        {
            Classes = { "floating" },
            Width = 210,
            Padding = new Thickness(12),
            Child = new TextBlock { Text = "floating child" }
        };
        var glassOnly = new Border
        {
            Classes = { "glass" },
            Width = 210,
            Padding = new Thickness(12),
            Child = new TextBlock { Text = "glass child" }
        };
        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 10,
            Children = { floatingOnly, glassOnly }
        };
        var window = new Window { Width = 500, Height = 140, Content = row };
        window.Show();
        Dispatcher.UIThread.RunJobs();
        window.UpdateLayout();

        using var rtb = new RenderTargetBitmap(new PixelSize(1000, 280), new Vector(192, 192));
        rtb.Render(window);
        rtb.Save(Path.Combine(Path.GetTempPath(), "liquidglass_visual_dump.png"));
        window.Close();
    }
}
