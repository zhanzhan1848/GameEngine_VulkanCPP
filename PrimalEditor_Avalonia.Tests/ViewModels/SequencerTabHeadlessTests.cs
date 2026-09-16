using System;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Presenters;
using Avalonia.Controls.Templates;
using Avalonia.Headless;
using Avalonia.Markup.Xaml.Styling;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;
using Avalonia.VisualTree;
using Dock.Avalonia.Controls;
using Dock.Avalonia.Themes.Fluent;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

/// <summary>
/// 与真实 App.axaml 同构的最小 Application：ViewLocator DataTemplate +
/// DockFluentTheme（DockControl 的控件模板来源）+ Warcraft 资源。
/// </summary>
internal sealed class DockHeadlessTestApp : Application
{
    public override void Initialize()
    {
        RequestedThemeVariant = ThemeVariant.Dark;
        DataTemplates.Add(new PrimalEditor_Avalonia.ViewLocator());
        Resources.MergedDictionaries.Add(new ResourceInclude(new Uri(
            "avares://PrimalEditor_Avalonia/Themes/Warcraft/WarcraftTheme.axaml"))
        {
            Source = new Uri("avares://PrimalEditor_Avalonia/Themes/Warcraft/WarcraftTheme.axaml")
        });
        Styles.Add(new FluentTheme());
        Styles.Add(new DockFluentTheme());
        // 与真实 App.axaml 一致：覆盖 Dock 的 DocumentContentControl 模板
        // （同步呈现），必须排在 DockFluentTheme 之后。
        Styles.Add(new StyleInclude(new Uri("avares://PrimalEditor_Avalonia/"))
        {
            Source = new Uri("avares://PrimalEditor_Avalonia/Themes/Controls/DocumentContentControlOverride.axaml")
        });
        Styles.Add(new StyleInclude(new Uri("avares://PrimalEditor_Avalonia/"))
        {
            Source = new Uri("avares://PrimalEditor_Avalonia/Themes/Warcraft/WarcraftTheme.Styles.axaml")
        });
        Styles.Add(new StyleInclude(new Uri("avares://PrimalEditor_Avalonia/"))
        {
            Source = new Uri("avares://PrimalEditor_Avalonia/Themes/Controls/CyberCorner.axaml")
        });
    }
}

/// <summary>
/// Toggle Sequencer 标签页的视图层回归：模型层 VisibleDockables 变化之后，
/// DockControl 的内容区必须真正物化出 SequencerPanel（曾因 VisibleDockables
/// 用普通 List + 绕过 Owner 语义导致标签/内容都不出现）。
/// </summary>
public sealed class SequencerTabHeadlessTests
{
    [Fact]
    public async System.Threading.Tasks.Task ToggleSequencer_MaterializesSequencerPanelInContent()
    {
        using var session = Avalonia.Headless.HeadlessUnitTestSession.StartNew(typeof(DockHeadlessTestApp));
        await session.Dispatch(() =>
        {
            var vm = new MainWindowViewModel();
            // 镜像真实启动：ThemeManager 会替换全局资源与样式。
            PrimalEditor_Avalonia.Themes.ThemeManager.Apply(
                PrimalEditor_Avalonia.Themes.EditorThemeRegistry.WarcraftId);
            var dock = new DockControl { Factory = vm.Factory, Layout = vm.Layout };
            var window = new Window { Content = dock, Width = 1200, Height = 800 };
            window.Show();
            Avalonia.Threading.Dispatcher.UIThread.RunJobs();

            try
            {
                var viewportBefore = window.GetVisualDescendants()
                    .OfType<PrimalEditor_Avalonia.Views.Engine3D.EngineViewport3D>().FirstOrDefault();
                Assert.NotNull(viewportBefore);
                var boundsBefore = viewportBefore!.Bounds;
                var originBefore = viewportBefore.TranslatePoint(default, window);
                var attached = 0;
                var detached = 0;
                viewportBefore.AttachedToVisualTree += (_, _) => attached++;
                viewportBefore.DetachedFromVisualTree += (_, _) => detached++;
                Assert.False(vm.IsSequencerVisible);
                vm.ToggleSequencerTab();
                Avalonia.Threading.Dispatcher.UIThread.RunJobs();
                window.UpdateLayout();

                // 标签条上要有 Sequencer 页签。
                var tabTexts = window.GetVisualDescendants()
                    .OfType<Avalonia.Controls.TextBlock>()
                    .Select(t => t.Text).ToList();
                Assert.Contains("Sequencer", tabTexts);

                // 内容区必须物化出真实的 SequencerPanel（曾为空的原因排查点）。
                var panel = window.GetVisualDescendants()
                    .OfType<PrimalEditor_Avalonia.Views.Sequencer.SequencerPanel>()
                    .FirstOrDefault();
                if (panel == null)
                {
                    var contentTypes = string.Join(", ", window.GetVisualDescendants()
                        .Where(v => v.Parent is ContentPresenter || v.Parent is ContentControl)
                        .Select(v => v.GetType().FullName)
                        .Distinct());
                    throw new Xunit.Sdk.XunitException(
                        $"SequencerPanel 未物化。Workspace: {string.Join(";", window.GetVisualDescendants().OfType<PrimalEditor_Avalonia.Views.DocumentWorkspace>().Select(w => $"dc={w.DataContext?.GetType().Name},active={w.ActiveDocument?.Id},children={w.Children.Count}"))} 内容区实际物化的控件: {contentTypes}");
                }
                Assert.True(panel.Bounds.Width > 600);
                Assert.True(panel.Bounds.Height > 150);
                Assert.True(panel.IsEffectivelyVisible);
                Assert.Same(viewportBefore, window.GetVisualDescendants()
                    .OfType<PrimalEditor_Avalonia.Views.Engine3D.EngineViewport3D>().Single());
                Assert.True(viewportBefore!.IsEffectivelyVisible);
                Assert.True(viewportBefore.Bounds.Height >= 200);
                Assert.NotNull(panel.FindControl<Button>("NewButton"));
                Assert.Equal(boundsBefore, viewportBefore.Bounds);
                Assert.Equal(originBefore, viewportBefore.TranslatePoint(default, window));
                Assert.Equal(0, attached);
                Assert.Equal(0, detached);
                // 切回 World Editor 后保留原视口实例。
                vm.ToggleSequencerTab();
                Avalonia.Threading.Dispatcher.UIThread.RunJobs();
                window.UpdateLayout();
                Assert.Same(viewportBefore, window.GetVisualDescendants()
                    .OfType<PrimalEditor_Avalonia.Views.Engine3D.EngineViewport3D>().FirstOrDefault());
                Assert.Null(window.GetVisualDescendants()
                    .OfType<PrimalEditor_Avalonia.Views.Sequencer.SequencerPanel>()
                    .FirstOrDefault());

                // Use the local tab itself for the next switch (two-way binding).
                var world = window.GetVisualDescendants().OfType<PrimalEditor_Avalonia.Views.WorldEditorView>().Single();
                world.FindControl<TabControl>("BottomPanelTabs")!.SelectedIndex = 1;
                Avalonia.Threading.Dispatcher.UIThread.RunJobs();
                window.UpdateLayout();
                Assert.True(vm.IsSequencerVisible);
                Assert.Equal(boundsBefore, viewportBefore.Bounds);
                Assert.Equal(originBefore, viewportBefore.TranslatePoint(default, window));
                Assert.Equal(0, attached);
                Assert.Equal(0, detached);
                Assert.NotNull(window.GetVisualDescendants()
                    .OfType<PrimalEditor_Avalonia.Views.Sequencer.SequencerPanel>()
                    .FirstOrDefault());

            }
            finally
            {
                window.Close();
            }
        }, System.Threading.CancellationToken.None);
    }
}
