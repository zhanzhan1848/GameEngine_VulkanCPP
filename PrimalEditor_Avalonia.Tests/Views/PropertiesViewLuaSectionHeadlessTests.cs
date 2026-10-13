using System;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Threading;
using Avalonia.VisualTree;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;
using PrimalEditor_Avalonia.Views;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Views;

/// <summary>
/// scripting S1 收官 D 门的自动化部分：Inspector 的 LUA SCRIPT 段必须在
/// 选中带 LuaScriptComponent 的实体时真正物化（应用按钮/参数行/添加行），
/// 无组件时不渲染。绑定语义（行集↔组件）由 VM 测试覆盖，这里只验视图层。
/// </summary>
public sealed class PropertiesViewLuaSectionHeadlessTests
{
    [Fact]
    public async Task LuaSection_MaterializesOnlyWhenComponentPresent()
    {
        using var session = HeadlessUnitTestSession.StartNew(typeof(ViewModels.DockHeadlessTestApp));
        await session.Dispatch(() =>
        {
            var vm = new PropertiesViewModel();
            var entity = new GameEntity("spinner");
            entity.Components.Add(new Transform());
            var lua = new LuaScriptComponent { ScriptName = "rotate.lua" };
            lua.Parameters["speed"] = "0.8";
            entity.Components.Add(lua);

            var view = new PropertiesView { DataContext = vm };
            var window = new Window { Content = view, Width = 480, Height = 1600 };
            window.Show();
            Dispatcher.UIThread.RunJobs();
            window.UpdateLayout();

            try
            {
                // 无选中 → 段不可见
                Assert.False(FindApplyButton(view)?.IsEffectivelyVisible ?? false);

                // 选中带脚本的实体 → 应用按钮 + 参数行 + 添加行都物化
                vm.Entities = new[] { entity };
                Dispatcher.UIThread.RunJobs();
                window.UpdateLayout();

                Assert.True(FindApplyButton(view)?.IsEffectivelyVisible ?? false);
                Assert.Contains(view.GetVisualDescendants().OfType<TextBox>(),
                    tb => tb.Watermark == "新参数名");
                // 组件参数行（speed=0.8）已渲染为行
                Assert.Contains(view.GetVisualDescendants().OfType<TextBox>(),
                    tb => tb.Text == "speed");
                Assert.Contains(view.GetVisualDescendants().OfType<TextBox>(),
                    tb => tb.Text == "0.8");

                // 换成无脚本实体 → 段收起
                var plain = new GameEntity("plain");
                plain.Components.Add(new Transform());
                vm.Entities = new[] { plain };
                Dispatcher.UIThread.RunJobs();
                window.UpdateLayout();
                Assert.False(FindApplyButton(view)?.IsEffectivelyVisible ?? false);
            }
            finally
            {
                window.Close();
            }
        }, System.Threading.CancellationToken.None);
    }

    private static Button? FindApplyButton(PropertiesView view) =>
        view.GetVisualDescendants().OfType<Button>()
            .FirstOrDefault(b => b.Content is string s && s.StartsWith("应用参数"));
}
