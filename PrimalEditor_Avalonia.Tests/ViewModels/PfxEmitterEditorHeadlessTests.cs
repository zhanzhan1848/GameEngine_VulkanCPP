using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Views.PFX;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

/// <summary>
/// PFX 编辑器的 UI 层回归（headless）：真实 PfxEmitterEditorView + 真实绑定。
/// 模拟用户在参数框里输入文本（TextBox.Text 走 TwoWay 绑定推送，与键入同管线），
/// 断言：脏标记立起 → Save 按钮启用 → 点击 Save 落盘为引擎可编译的 JSON。
/// 锁死"改了参数却无法保存/重进即重置"这条链路。
/// </summary>
[Collection("ThemeHeadlessSerial")]
public sealed class PfxEmitterEditorHeadlessTests
{
    private const string SampleJson = """
    {
      "name": "dust_puff",
      "blend": "alpha",
      "capacity": 4096,
      "stages": {
        "spawn": [ { "module": "SpawnRate", "rate": 50 } ],
        "init": [
          { "module": "SetLifetime", "min": 1.0, "max": 2.5 },
          { "module": "SetScale", "scale": [0.3, 0.3], "scale_end": [0.8, 0.8] }
        ],
        "update": []
      }
    }
    """;

    private static void InjectSchema()
    {
        PrimalEditor_Avalonia.Services.PfxEditorService.Instance.SetSchema(
            new PrimalEditor_Avalonia.Models.PFX.PfxSchema
            {
                Version = 1,
                Stages = { "spawn", "init", "update" },
                Modules =
                {
                    new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
                    {
                        Name = "SpawnRate", Stage = "spawn",
                        Params = { new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "rate", Type = "param_ref", Note = "particles per second" } },
                    },
                    new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
                    {
                        Name = "SetLifetime", Stage = "init",
                        Params =
                        {
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "min", Type = "param_ref", Note = "seconds" },
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "max", Type = "param_ref", Note = "seconds" },
                        },
                    },
                    new PrimalEditor_Avalonia.Models.PFX.PfxModuleSchema
                    {
                        Name = "SetScale", Stage = "init",
                        Params =
                        {
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "scale", Type = "v2", Note = "[w,h]" },
                            new PrimalEditor_Avalonia.Models.PFX.PfxParamSchema { Name = "scale_end", Type = "v2", Note = "optional end (random lerp)" },
                        },
                    },
                },
            });
    }

    private static IEnumerable<T> Descendants<T>(Visual root) where T : Visual
    {
        var stack = new Stack<Visual>();
        stack.Push(root);
        while (stack.Count > 0)
        {
            var v = stack.Pop();
            if (v is T match) yield return match;
            foreach (var child in v.GetVisualChildren()) stack.Push(child);
        }
    }

    [Fact]
    public async Task TypedParamEdit_EnablesSaveButton_AndPersistsNumberOnSaveClick()
    {
        var session = HeadlessUnitTestSession.StartNew(typeof(ThemeHeadlessTestApp));
        await session.Dispatch(() =>
        {
            InjectSchema();
            var path = Path.Combine(Path.GetTempPath(), $"pfx_headless_{Guid.NewGuid():N}.pfx_emitter");
            File.WriteAllText(path, SampleJson);
            try
            {
                var vm = new PrimalEditor_Avalonia.ViewModels.PFX.PfxEmitterEditorViewModel();
                vm.SetAsset(new AssetInfo
                {
                    FullPath = path,
                    Type = PrimalEditor_Avalonia.Native.AssetType.PfxEmitter,
                });

                var view = new PfxEmitterEditorView { DataContext = vm };
                var window = new Window { Content = view, Width = 1500, Height = 950 };
                window.Show();
                Dispatcher.UIThread.RunJobs();
                try
                {
                    Assert.False(vm.IsDirty);
                    var saveButton = Descendants<Button>(view).First(b => b.Content as string == "Save");
                    var discardButton = Descendants<Button>(view).First(b => b.Content as string == "Discard");
                    Assert.False(saveButton.IsEnabled, "fresh load must leave Save disabled");
                    Assert.False(discardButton.IsEnabled);

                    // 模拟真实键入：改写 rate 参数框（TwoWay 绑定推送，与逐键输入同管线）
                    var rateBox = Descendants<TextBox>(view)
                        .First(tb => ToolTip.GetTip(tb) as string == "particles per second");
                    Assert.Equal("50", rateBox.Text);

                    // 诊断：记录 VM 的 INPC 事件（经基类 ObservableObject 的
                    // PropertyChanged 事件槽，XAML 绑定同听此事件）
                    var inpcEvents = new List<string>();
                    System.ComponentModel.PropertyChangedEventHandler handler = (_, e) => inpcEvents.Add(e.PropertyName);
                    ((System.ComponentModel.INotifyPropertyChanged)vm).PropertyChanged += handler;
                    rateBox.Text = "200";
                    Dispatcher.UIThread.RunJobs();

                    Assert.True(vm.IsDirty, "typed param edit must raise the dirty flag");
                    Assert.Equal("200", vm.AllModules.Single(m => m.Name == "SpawnRate")
                        .Params.Single(p => p.Name == "rate").Value);

                    // 回归门禁（INPC 事件槽）：XAML 绑定通过 INotifyPropertyChanged
                    // 接口订阅。此前 PfxEmitterEditorViewModel 重新声明了 new
                    // event PropertyChanged（接口映射仍落在基类 ObservableObject
                    // 的事件上），Fire 触发的是派生事件槽 → 绑定永远收不到
                    // PropertyChanged → Save/Discard 永远禁用。此探针沿接口
                    // 订阅；若事件槽再次分裂，这里将收不到 "IsDirty"。
                    Assert.Contains("IsDirty", inpcEvents);
                    Assert.True(saveButton.IsEnabled, "Save must enable once dirty");
                    Assert.True(discardButton.IsEnabled);

                    // 真实点击 Save（走输入管线的 Click 路径）
                    var center = new Point(saveButton.Bounds.Width / 2, saveButton.Bounds.Height / 2);
                    var m = saveButton.TransformToVisual(window)
                        ?? throw new InvalidOperationException("Save button not laid out");
                    var screenPoint = m.Transform(center);
                    window.MouseDown(screenPoint, MouseButton.Left);
                    window.MouseUp(screenPoint, MouseButton.Left);
                    Dispatcher.UIThread.RunJobs();

                    Assert.False(vm.IsDirty, "Save click must clear the dirty flag");
                    using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
                    var el = doc.RootElement.GetProperty("stages").GetProperty("spawn")[0].GetProperty("rate");
                    Assert.Equal(System.Text.Json.JsonValueKind.Number, el.ValueKind);
                    Assert.Equal(200f, el.GetSingle());
                }
                finally
                {
                    window.Close();
                }
            }
            finally { File.Delete(path); }
        }, CancellationToken.None);
    }
}
