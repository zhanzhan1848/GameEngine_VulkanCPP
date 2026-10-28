using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Cli;

namespace PrimalEditor_Avalonia.Gameplay;

/// <summary>
/// gameplay S3：共创回路的人在环决定面（最小确认面）。
/// 每提案恰好三个动作：批准（apply）/ 驳回（填原因回喂）/ 重新取证（dryrun）
/// ——"只调参数"不在此窗口（人经 Inspector/组件命令改 L0 数值+Undo，无需 AI）。
/// 数据全部来自 proposals/ 目录（gameplay.* 命令同一事实源）；
/// 本窗口只是命令面的 UI 壳——动作直接调 GameplayCommands，无第二逻辑。
/// </summary>
public sealed class GameplayReviewWindow : Window
{
    private readonly GameplayCommands _commands = new();
    private readonly GameplayEvolutionCommands _evolution = new();
    private readonly StackPanel _list = new() { Spacing = 8, Margin = new Avalonia.Thickness(12) };
    private readonly TextBlock _status = new() { FontSize = 11, Opacity = 0.8, Margin = new Avalonia.Thickness(12, 0, 12, 8) };

    public GameplayReviewWindow()
    {
        Title = "Gameplay 共创回路 — 决定面";
        Width = 640; Height = 520;
        Content = new Grid { RowDefinitions = new Avalonia.Controls.RowDefinitions("*,Auto,Auto"),
            Children = { _list } };
        if (Content is Grid g)
        {
            Grid.SetRow(_list, 0);
            Grid.SetRow(_status, 1);
            var close = new Button { Content = "关闭", HorizontalAlignment = HorizontalAlignment.Right,
                Margin = new Avalonia.Thickness(12, 0, 12, 12) };
            close.Click += (_, _) => Close();
            Grid.SetRow(close, 2);
            g.Children.Add(_status);
            g.Children.Add(close);
        }
        Refresh();
    }

    private string ProjectDir =>
        Cli.EditorServices.WorldEditor?.Project?.Path
        ?? throw new InvalidOperationException("项目未加载");

    public void Refresh()
    {
        _list.Children.Clear();
        try
        {
            var status = _commands.Status() as dynamic;
            _status.Text = $"提案目录：{status.root} · 动作=批准/驳回/重取证（人改数值走 Inspector+Undo）";
            var items = (System.Collections.IEnumerable)status.proposals;
            int shown = 0;
            foreach (var item in items)
            {
                if (shown++ >= 16) break;
                _list.Children.Add(BuildRow(item));
            }
            if (shown == 0)
                _list.Children.Add(new TextBlock
                {
                    Text = "没有提案。AI 经 gameplay.propose 登记（或 MCP/CLI 同名命令）；此处只做人审。",
                    FontSize = 12, Opacity = 0.7, Margin = new Avalonia.Thickness(0, 24, 0, 0),
                });
        }
        catch (Exception ex)
        {
            _list.Children.Add(new TextBlock { Text = $"读提案失败：{ex.Message}", Foreground = Brushes.IndianRed });
        }
    }

    private Control BuildRow(dynamic item)
    {
        var name = (string)item.name;
        var status = (string)item.status;
        var files = (int)item.files;
        var hasVerdict = (bool)item.hasVerdict;

        var border = new Border
        {
            BorderBrush = Brushes.Gray, BorderThickness = new Avalonia.Thickness(1),
            CornerRadius = new Avalonia.CornerRadius(4), Padding = new Avalonia.Thickness(8),
        };
        var head = new TextBlock
        {
            FontSize = 13, FontWeight = FontWeight.Bold,
            Text = $"{name}  [{status}]  files={files}" + (hasVerdict ? "  · 有证据" : "  · 无证据（先 dryrun）"),
        };
        var reasonBox = new TextBox { Watermark = "驳回原因（回喂 AI 迭代）", FontSize = 11, MinHeight = 30, TextWrapping = Avalonia.Media.TextWrapping.Wrap };
        var applyBtn = new Button { Content = "批准生效", FontSize = 11, Padding = new Avalonia.Thickness(10, 2) };
        var dryBtn = new Button { Content = "重新取证", FontSize = 11, Padding = new Avalonia.Thickness(10, 2) };
        var rejectBtn = new Button { Content = "驳回", FontSize = 11, Padding = new Avalonia.Thickness(10, 2) };
        applyBtn.Click += (_, _) => RunAction(() => _commands.Apply(name), applyBtn);
        dryBtn.Click += (_, _) => RunAction(() => { DryRunOffUi(name); return new { started = true }; }, dryBtn);
        rejectBtn.Click += (_, _) =>
        {
            if (string.IsNullOrWhiteSpace(reasonBox.Text)) { _status.Text = "驳回必须填原因（回喂回路的上下文）"; return; }
            RunAction(() => _evolution.Reject(name, reasonBox.Text.Trim()), rejectBtn);
        };
        var row = new StackPanel { Spacing = 6 };
        row.Children.Add(head);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        buttons.Children.Add(dryBtn);
        buttons.Children.Add(applyBtn);
        buttons.Children.Add(rejectBtn);
        row.Children.Add(buttons);
        row.Children.Add(reasonBox);
        border.Child = row;
        return border;
    }

    private void DryRunOffUi(string name)
    {
        // Player 是外部进程，等它退出会冻结 UI——丢线程池，完成回 UI 线程刷列表。
        System.Threading.Tasks.Task.Run(() =>
        {
            try { _commands.DryRun(name); }
            catch { /* verdict/错误已落 dryrun 目录（verdict.json/stderr.log） */ }
            Dispatcher.UIThread.Post(Refresh);
        });
    }

    private void RunAction(Func<object> action, Control source)
    {
        _status.Text = "执行中…";
        try
        {
            var result = action();
            _status.Text = result switch
            {
                string s => s,
                _ => JsonSerializer.Serialize(result, new JsonSerializerOptions
                {
                    WriteIndented = false,
                    DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull,
                }) is { Length: > 0 } js && js.Length <= 400 ? js : "完成",
            };
        }
        catch (CliError e)
        {
            _status.Text = $"{e.Code}: {e.Message}";
        }
        catch (Exception e)
        {
            _status.Text = $"失败：{e.Message}";
        }
        Refresh();
    }
}
