using System;
using System.IO;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Headless;
using Avalonia.Markup.Xaml;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Skia;
using Avalonia.Threading;
using Avalonia.VisualTree;
using PrimalEditor_Avalonia.Themes;
using PrimalEditor_Avalonia.Themes.Controls;
using Xunit;

namespace PrimalEditor_Avalonia.Tests;

/// <summary>
/// 非提交性的视觉检查工具:在 headless + Skia 渲染会话里套用 Warcraft 主题,
/// 渲染典型部件(纹理瓦片/材质节点外框/按钮)并输出 PNG 到临时目录,供人工核对。
/// 运行: dotnet test --filter FullyQualifiedName~WarcraftVisualDump
/// </summary>
[Collection("ThemeHeadlessSerial")]
public class WarcraftVisualDump
{
    [Fact]
    public async System.Threading.Tasks.Task Dump_WarcraftFrames()
    {
        var session = HeadlessUnitTestSession.StartNew(typeof(ThemeHeadlessTestApp));
        await session.Dispatch(() =>
        {
            ThemeManager.Apply(EditorThemeRegistry.WarcraftId);
            Dispatcher.UIThread.RunJobs();

            var folderGeom = StreamGeometry.Parse("M0,12 L0,38 L0,40 L2,40 L38,40 L40,40 L40,16 L40,14 L22,14 L18,8 L18,8 L18,6 L4,6 L2,6 L0,8 L0,12 Z");

            System.Func<string, TextureFrame> makeTile = name => new TextureFrame
            {
                Classes = { "floating", "tex-chip" },
                Width = 68, Height = 52,
                Child = new StackPanel
                {
                    HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center,
                    VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center,
                    Children =
                    {
                        new Panel
                        {
                            Width = 20, Height = 20,
                            Children =
                            {
                                new Avalonia.Controls.Shapes.Path
                                {
                                    Width = 14, Height = 14, Stretch = Stretch.Uniform,
                                    Data = folderGeom,
                                    Fill = new SolidColorBrush(Color.FromRgb(0xE8, 0xB5, 0x4A)),
                                    HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center,
                                    VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center,
                                },
                            }
                        },
                        new TextBlock { Text = name, FontSize = 9, HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center },
                    }
                },
            };

            // 材质编辑器节点同款: NodifyM Node, 主题样式附加九宫格外框
            var node = new NodifyM.Avalonia.Controls.Node
            {
                Header = "Constant Float3",
                Width = 220, Height = 90,
                Background = new SolidColorBrush(Color.FromRgb(0x2E, 0x26, 0x18)),
                Content = new StackPanel
                {
                    // 与 MaterialEditorView 一致: 阵营头像行 + 参数行
                    Children =
                    {
                        new StackPanel
                        {
                            Orientation = Avalonia.Layout.Orientation.Horizontal,
                            Margin = new Thickness(12, 8, 12, 2),
                            IsVisible = true,
                            Children =
                            {
                                new Image
                                {
                                    Width = 18, Height = 18, VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center,
                                    Source = PrimalEditor_Avalonia.Themes.Controls.NineSliceImage.LoadSource(
                                        "avares://PrimalEditor_Avalonia/Assets/Textures/Warcraft/avatar-orc.png"),
                                },
                                new TextBlock { Text = "orc / Add", FontSize = 10, VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center, Margin = new Thickness(6, 0, 0, 0) },
                            }
                        },
                        new TextBlock { Text = "value   1    0    0", Margin = new Thickness(16, 10, 16, 10) },
                    }
                },
            };

            var focusRun = new Button { Content = "Run", Classes = { "accent" }, Width = 90, Height = 30, Margin = new Thickness(0, 8, 0, 0) };
            var normal = new Button { Content = "Build", Width = 90, Height = 30, Margin = new Thickness(0, 8, 0, 0) };

            var row = new StackPanel
            {
                Orientation = Avalonia.Layout.Orientation.Horizontal,
                Children = { makeTile("Assets"), makeTile("Metarial") },
            };
            var column = new StackPanel { Background = Brushes.Black, Children = { row, node, focusRun, normal } };

            node.DataContext = new { TypeName = "Add" };

            var window = new Window { Width = 220, Height = 260, Content = column, Background = Brushes.Black };
            window.Show();
            Dispatcher.UIThread.RunJobs();
            window.UpdateLayout();

            focusRun.Focus();
            Dispatcher.UIThread.RunJobs();
            window.UpdateLayout();

            using var rtb = new RenderTargetBitmap(new PixelSize(880, 1040), new Vector(192, 192));
            rtb.Render(window);
            rtb.Save(System.IO.Path.Combine(System.IO.Path.GetTempPath(), "warcraft_visual_dump.png"));

            // 还原默认主题(Warcraft),避免静态 ThemeManager 状态泄漏到
            // 同进程的其它测试会话(它们都以默认主题启动)
            ThemeManager.Apply(EditorThemeRegistry.WarcraftId);
            window.Close();
        }, System.Threading.CancellationToken.None);
    }
}
