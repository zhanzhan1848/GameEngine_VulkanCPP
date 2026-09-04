using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Markup.Xaml;
using Avalonia.Markup.Xaml.Styling;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Media.Immutable;
using Avalonia.Styling;
using Avalonia.Threading;
using Avalonia.Skia;
using Avalonia.Themes.Fluent;
using PrimalEditor_Avalonia.Themes;
using PrimalEditor_Avalonia.Themes.Controls;
using Xunit;

namespace PrimalEditor_Avalonia.Tests;

[CollectionDefinition("ThemeHeadlessSerial", DisableParallelization = true)]
public class ThemeHeadlessSerialCollection { }

/// <summary>
/// Minimal Application for headless tests: FluentTheme + the real Cyberpunk
/// boot theme, mirroring App.axaml without the editor's engine/CLI startup.
/// </summary>
internal sealed class ThemeTestApp : Application
{
    public override void Initialize()
    {
        Resources.MergedDictionaries.Add(new ResourceInclude(new Uri(
            "avares://PrimalEditor_Avalonia/Themes/Warcraft/WarcraftTheme.axaml"))
        {
            Source = new Uri("avares://PrimalEditor_Avalonia/Themes/Warcraft/WarcraftTheme.axaml")
        });
        Styles.Add(new FluentTheme());
        // 与 App.axaml 一致: NodifyM 在前, 主题样式在后(后者覆写前者模板)
        Styles.Add(new StyleInclude(new Uri("avares://PrimalEditor_Avalonia/"))
        {
            Source = new Uri("avares://NodifyM.Avalonia/Styles/ControlStyles.axaml")
        });
        Styles.Add(new StyleInclude(new Uri("avares://PrimalEditor_Avalonia/"))
        {
            Source = new Uri("avares://PrimalEditor_Avalonia/Themes/Warcraft/WarcraftTheme.Styles.axaml")
        });
    }
}

public static class ThemeHeadlessTestApp
{
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<ThemeTestApp>()
            .UseSkia()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false });
}

/// <summary>
/// Runtime theme switching against LIVE controls (the path the View menu
/// uses): after <see cref="ThemeManager.Apply"/> both token resources
/// (DynamicResource) and global styles must re-resolve on shown controls,
/// and the choice must persist. Runs on a headless Avalonia session —
/// no GPU/windowing needed.
/// </summary>
public class ThemeManagerHeadlessTests
{
    [Fact]
    public async System.Threading.Tasks.Task Apply_WithLiveWindow_UpdatesTokensStylesAndPersists()
    {
        var session = HeadlessUnitTestSession.StartNew(typeof(ThemeHeadlessTestApp));
        await session.Dispatch(() =>
        {
            var border = new Border();
            border.Bind(Border.BackgroundProperty, border.GetResourceObservable("PanelBrush"));
            var text = new TextBlock();
            var window = new Window { Content = new StackPanel { Children = { border, text } } };
            window.Show();
            Dispatcher.UIThread.RunJobs();

            try
            {
                // Boot theme = Warcraft (the new default): parchment text on
                // iron panels. Capture it, then switch away and back.
                var panelBefore = ((ISolidColorBrush)border.Background!).Color;
                var textBefore = ((ISolidColorBrush)text.Foreground!).Color;
                Assert.Equal(Color.Parse("#E8DCC0"), textBefore);

                Assert.True(ThemeManager.Apply(EditorThemeRegistry.CyberpunkId));
                Assert.Equal(EditorThemeRegistry.CyberpunkId, ThemeManager.Current.Id);
                Dispatcher.UIThread.RunJobs();

                // Token dictionary swap: DynamicResource consumers re-resolve.
                var panelAfter = ((ISolidColorBrush)border.Background!).Color;
                Assert.NotEqual(panelBefore, panelAfter);

                // Global styles swap: the TextBlock style restyles Foreground.
                var textAfter = ((ISolidColorBrush)text.Foreground!).Color;
                Assert.NotEqual(textBefore, textAfter);

                // Switching back restores the boot look (styles re-resolve again).
                Assert.True(ThemeManager.Apply(EditorThemeRegistry.WarcraftId));
                Dispatcher.UIThread.RunJobs();
                Assert.Equal(panelBefore, ((ISolidColorBrush)border.Background!).Color);
                Assert.Equal(textBefore, ((ISolidColorBrush)text.Foreground!).Color);

                // The choice is persisted (to the test-redirected settings file).
                Assert.Equal(EditorThemeRegistry.WarcraftId, EditorThemeSettings.Load().ThemeId);
            }
            finally
            {
                window.Close();
            }
        }, System.Threading.CancellationToken.None);
    }

    [Fact]
    public async System.Threading.Tasks.Task WarcraftTextureAssets_LoadAndRender()
    {
        var session = HeadlessUnitTestSession.StartNew(typeof(ThemeHeadlessTestApp));
        await session.Dispatch(() =>
        {
            foreach (var name in new[] { "button-normal", "button-hover", "button-pressed",
                                         "button-accent", "button-accent-hover", "button-focus", "button-accent-focus",
                                         "frame-strip", "frame-panel",
                                         "avatar-orc", "avatar-human", "avatar-elf", "avatar-undead", "avatar-default",
                                         "node-frame-orc", "node-frame-human", "node-frame-elf", "node-frame-undead", "node-frame-default" })
            {
                var uri = $"avares://PrimalEditor_Avalonia/Assets/Textures/Warcraft/{name}.png";
                Assert.NotNull(NineSliceImage.LoadSource(uri));
            }

            // A frame with art renders without throwing; without art it falls
            // back to plain Border rendering (Cyberpunk behaviour).
            var frame = new TextureFrame { Width = 200, Height = 60, Source = "avares://PrimalEditor_Avalonia/Assets/Textures/Warcraft/frame-strip.png" };
            var plain = new TextureFrame { Width = 200, Height = 60 };
            var panel = new Panel { Children = { frame, plain } };
            panel.Measure(new Size(200, 60));
            panel.Arrange(new Rect(0, 0, 200, 60));
            using var bmp = new RenderTargetBitmap(new PixelSize(200, 60));
            using (var ctx = bmp.CreateDrawingContext())
            {
                frame.Render(ctx);
                plain.Render(ctx);
            }
        }, System.Threading.CancellationToken.None);
    }

    [Fact]
    public async System.Threading.Tasks.Task RuntimeAddedStyles_ApplyToShownWindow_Diagnostic()
    {
        var session = HeadlessUnitTestSession.StartNew(typeof(ThemeHeadlessTestApp));
        await session.Dispatch(() =>
        {
            var text = new TextBlock();
            var window = new Window { Content = text };
            window.Show();
            Application.Current!.Styles.Add(new Styles
            {
                new Style(x => x.OfType<TextBlock>())
                {
                    Setters = { new Setter(TextBlock.ForegroundProperty, Brushes.Goldenrod) }
                }
            });
            Dispatcher.UIThread.RunJobs();
            window.UpdateLayout();
            var fg = ((ISolidColorBrush)text.Foreground!).Color;
            window.Close();
            Assert.Equal(Brushes.Goldenrod.Color, fg);
        }, System.Threading.CancellationToken.None);
    }

    [Fact]
    public async System.Threading.Tasks.Task Apply_UnknownThemeId_ReturnsFalseAndKeepsCurrent()
    {
        var session = HeadlessUnitTestSession.StartNew(typeof(ThemeHeadlessTestApp));
        await session.Dispatch(() =>
        {
            var current = ThemeManager.Current;
            Assert.False(ThemeManager.Apply("does-not-exist"));
            Assert.Same(current, ThemeManager.Current);
        }, System.Threading.CancellationToken.None);
    }
}

/// <summary>
/// Liquid Glass 主题的运行时切换验证。放在 DisableParallelization 集合里:
/// ThemeManager 是进程级静态状态,与其它 headless 会话并行 Apply 会互相踩踏。
/// </summary>
[Collection("ThemeHeadlessSerial")]
public class LiquidGlassThemeHeadlessTests
{
    [Fact]
    public async System.Threading.Tasks.Task Apply_LiquidGlass_SwapsToLightVariantGlassTokens()
    {
        var session = HeadlessUnitTestSession.StartNew(typeof(ThemeHeadlessTestApp));
        await session.Dispatch(() =>
        {
            var app = Application.Current!;
            var border = new Border();
            border.Bind(Border.BackgroundProperty, border.GetResourceObservable("PanelBrush"));
            var text = new TextBlock();
            var window = new Window { Content = new StackPanel { Children = { border, text } } };
            window.Show();
            Dispatcher.UIThread.RunJobs();

            try
            {
                var panelBefore = ((ISolidColorBrush)border.Background!).Color;

                Assert.True(ThemeManager.Apply(EditorThemeRegistry.LiquidGlassId));
                Assert.Equal(EditorThemeRegistry.LiquidGlassId, ThemeManager.Current.Id);
                Dispatcher.UIThread.RunJobs();

                // 浅色皮肤必须把 Fluent 变体带到 Light(弹层/下拉/复选框跟随)
                Assert.Equal(ThemeVariant.Light, app.RequestedThemeVariant);
                Assert.Equal(ThemeVariant.Light, window.ActualThemeVariant);

                // 玻璃面板令牌精确生效(半透明白,叠在极光底板上读作玻璃)
                Assert.Equal(Color.Parse("#A6FFFFFF"), ((ISolidColorBrush)border.Background!).Color);
                Assert.NotEqual(panelBefore, ((ISolidColorBrush)border.Background!).Color);
                // 正文墨色: 深靛蓝墨水(浅色底上的可读性)
                Assert.Equal(Color.Parse("#212A4E"), ((ISolidColorBrush)text.Foreground!).Color);

                // 切回默认主题恢复启动外观与 Dark 变体
                Assert.True(ThemeManager.Apply(EditorThemeRegistry.CyberpunkId));
                Dispatcher.UIThread.RunJobs();
                Assert.Equal(ThemeVariant.Dark, app.RequestedThemeVariant);
                Assert.Equal(panelBefore, ((ISolidColorBrush)border.Background!).Color);
            }
            finally
            {
                window.Close();
            }
        }, System.Threading.CancellationToken.None);
    }
}
