using System;
using System.Globalization;
using System.IO;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Threading;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Cli;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Sequence;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views.Sequencer;

/// <summary>
/// Sequencer 面板 code-behind（Part A）：Canvas 自绘标尺/轨道/关键帧菱形/
/// 相机 cut 段/playhead，指针拖拽 scrub。按钮内部调用 sequence.* 命令方法
/// （与 CLI 同一 API 与 undo 路径——§5.2 "UI 按钮 = 对自家命令的内部调用"）。
/// 时间轴是帧号轴；鼠标 X → 帧号按 fit-to-width 换算（一期不做缩放持久化）。
/// </summary>
public partial class SequencerPanel : UserControl
{
    // 绘制度量
    private const double RulerHeight = 22;
    private const double RowHeight = 26;
    private const double NameColumnWidth = 170;
    private const double KeyHitRadius = 7;

    private static readonly IBrush PlayheadBrush = new SolidColorBrush(Color.FromRgb(0x00, 0xB4, 0xFF));
    private static readonly IBrush KeyBrush = new SolidColorBrush(Color.FromRgb(0xE8, 0xC4, 0x47));
    private static readonly IBrush CutBrush = new SolidColorBrush(Color.FromArgb(0x90, 0xE8, 0x7D, 0x3C));
    private static readonly IBrush RowBrush = new SolidColorBrush(Color.FromArgb(0x18, 0xFF, 0xFF, 0xFF));
    private static readonly IBrush RulerBrush = new SolidColorBrush(Color.FromArgb(0x30, 0xFF, 0xFF, 0xFF));
    private static readonly IBrush TextBrush = new SolidColorBrush(Color.FromRgb(0xC8, 0xD0, 0xD8));

    private readonly SequenceCommands _commands = new();
    private bool _scrubbing;
    private bool _renderInProgress;

    public SequencerPanel()
    {
        InitializeComponent();
        SequenceEditorState.Changed += OnSequenceChanged;
        SequenceCommands.Playback.PlaybackStopped += OnSequenceChanged;
        DetachedFromVisualTree += (_, _) =>
        {
            SequenceEditorState.Changed -= OnSequenceChanged;
            SequenceCommands.Playback.PlaybackStopped -= OnSequenceChanged;
        };
        Redraw();
    }

    private void OnSequenceChanged()
    {
        if (Dispatcher.UIThread.CheckAccess()) Redraw();
        else Dispatcher.UIThread.Post(Redraw);
    }

    // ===================================================================
    // 传输与文件按钮
    // ===================================================================

    private void OnNewClick(object? sender, RoutedEventArgs e)
    {
        Run(() => _commands.New(Text(NameBox), fps: 30, durationFrames: 120));
    }

    private async void OnOpenClick(object? sender, RoutedEventArgs e)
    {
        var topLevel = TopLevel.GetTopLevel(this);
        if (topLevel?.StorageProvider == null) return;
        var files = await topLevel.StorageProvider.OpenFilePickerAsync(new Avalonia.Platform.Storage.FilePickerOpenOptions
        {
            Title = "打开 Sequence",
            AllowMultiple = false,
            FileTypeFilter = new[] { new Avalonia.Platform.Storage.FilePickerFileType("Primal Sequence") { Patterns = new[] { "*.pseq" } } },
        });
        if (files.Count == 0) return;
        var path = files[0].Path.LocalPath;
        Run(() => _commands.Open(path));
    }

    private void OnSaveClick(object? sender, RoutedEventArgs e) => Run(() => _commands.Save());

    private void OnJumpStartClick(object? sender, RoutedEventArgs e) => Seek(0);
    private void OnPrevFrameClick(object? sender, RoutedEventArgs e) => Seek(SequenceEditorState.CurrentFrame - 1);
    private void OnNextFrameClick(object? sender, RoutedEventArgs e) => Seek(SequenceEditorState.CurrentFrame + 1);
    private void OnJumpEndClick(object? sender, RoutedEventArgs e) =>
        Seek((SequenceEditorState.Current?.DurationFrames ?? 1) - 1);

    private void OnPlayPauseClick(object? sender, RoutedEventArgs e)
    {
        if (SequenceEditorState.Current == null) return;
        if (SequenceCommands.Playback.IsRunning) SequenceCommands.Playback.Stop(rewind: false);
        else SequenceCommands.Playback.Start();
        Redraw();
    }

    // ===================================================================
    // 编辑按钮
    // ===================================================================

    private void OnKeyClick(object? sender, RoutedEventArgs e)
    {
        var entity = SelectedEntity();
        if (entity == null)
        {
            Logger.Warning("[sequencer] K 帧：先在层级面板选中实体");
            return;
        }
        Run(() => _commands.AddKey(entity.EntityGuid.Value));
    }

    private void OnAddTrackClick(object? sender, RoutedEventArgs e)
    {
        var entity = SelectedEntity();
        if (entity == null)
        {
            Logger.Warning("[sequencer] 加轨：先在层级面板选中实体");
            return;
        }
        Run(() => _commands.AddTransformTrack(entity.EntityGuid.Value));
    }

    private void OnAddCutClick(object? sender, RoutedEventArgs e)
    {
        var entity = SelectedEntity();
        if (entity == null || entity.Components.OfType<CameraComponent>().FirstOrDefault() == null)
        {
            Logger.Warning("[sequencer] + Cut：先选中带 Camera 组件的实体");
            return;
        }
        var start = SequenceEditorState.CurrentFrame;
        Run(() => _commands.AddCameraCut(start, start + 24, entity.EntityGuid.Value));
    }

    private async void OnRenderClick(object? sender, RoutedEventArgs e)
    {
        var doc = SequenceEditorState.Current;
        var worldEditor = EditorServices.WorldEditor;
        if (doc == null || worldEditor?.Project == null || _renderInProgress) return;
        if (EditorServices.FindViewport() == null)
        {
            Logger.Warning("[sequencer] 渲染：3D 视口未就绪");
            return;
        }
        var dir = Path.Combine(worldEditor.Project.Path, "Captures",
            $"{Sanitize(doc.Name)}-{DateTime.Now:yyyyMMdd-HHmmss}");
        var video = Path.Combine(dir, Sanitize(doc.Name) + ".mp4");
        _renderInProgress = true;
        RenderButton.IsEnabled = false;
        try
        {
            var result = await _commands.Render(dir, video);
            Logger.Info($"[sequencer] 渲染完成 → {dir}（mp4: {video}）");
            Logger.Debug(result?.ToString() ?? "");
        }
        catch (CliError error)
        {
            Logger.Error($"[sequencer] 渲染失败：{error.Message}（{error.Hint}）");
        }
        catch (Exception ex)
        {
            Logger.Error($"[sequencer] 渲染失败：{ex.Message}", ex);
        }
        finally
        {
            _renderInProgress = false;
            RenderButton.IsEnabled = true;
        }
    }

    // ===================================================================
    // Canvas 绘制
    // ===================================================================

    private double PixelsPerFrame => double.IsNaN(TimelineCanvas.Bounds.Width) || TimelineCanvas.Bounds.Width <= NameColumnWidth + 16
        ? 4.0
        : Math.Max(1.0, (TimelineCanvas.Bounds.Width - NameColumnWidth - 8) / Math.Max(1, SequenceEditorState.Current?.DurationFrames ?? 120));

    private double FrameToX(int frame) => NameColumnWidth + frame * PixelsPerFrame;

    private int XToFrame(double x)
    {
        var doc = SequenceEditorState.Current;
        var max = (doc?.DurationFrames ?? 1) - 1;
        return Math.Clamp((int)Math.Round((x - NameColumnWidth) / PixelsPerFrame), 0, Math.Max(0, max));
    }

    private void Redraw()
    {
        var canvas = TimelineCanvas;
        if (canvas == null || !IsVisible) return;
        canvas.Children.Clear();
        var doc = SequenceEditorState.Current;
        FrameLabel.Text = doc == null
            ? "—"
            : $"F {SequenceEditorState.CurrentFrame} / {doc.DurationFrames} @ {doc.Fps}fps" +
              (SequenceCommands.Playback.IsRunning ? " ▶" : "");
        PlayPauseButton.Content = SequenceCommands.Playback.IsRunning ? "Pause" : "Play";
        if (doc == null)
        {
            DrawText(canvas, "sequence.new / Open… 创建或打开序列", 10, RulerHeight + 10);
            return;
        }

        var tracks = doc.Tracks;
        canvas.Height = Math.Max(110, RulerHeight + (tracks.Count + 1) * RowHeight + 8);
        var canvasWidth = double.IsFinite(canvas.Bounds.Width) ? canvas.Bounds.Width : 820;

        // 标尺（自适应刻度步长）
        var step = Math.Max(1, (int)Math.Round(60 / PixelsPerFrame));
        var nice = step <= 5 ? 5 : step <= 10 ? 10 : step <= 30 ? 30 : step <= 60 ? 60 : 120;
        for (int f = 0; f <= doc.DurationFrames; f += nice)
        {
            var x = FrameToX(f);
            canvas.Children.Add(Line(x, 0, x, canvas.Height, RulerBrush, 1));
            DrawText(canvas, f.ToString(), x + 3, 4);
        }

        // 相机 cut 行（第一行）
        double y = RulerHeight;
        canvas.Children.Add(Rect(NameColumnWidth, y + 2, canvasWidth - NameColumnWidth - 4, RowHeight - 4, RowBrush));
        if (doc.CameraCut != null)
        {
            DrawText(canvas, "Camera Cuts", 8, y + (RowHeight - 12) / 2);
            foreach (var shot in doc.CameraCut.Shots)
            {
                var x0 = FrameToX(shot.StartFrame);
                var x1 = FrameToX(Math.Min(shot.EndFrame, doc.DurationFrames));
                canvas.Children.Add(Rect(x0, y + 3, Math.Max(2, x1 - x0), RowHeight - 6, CutBrush));
            }
            y += RowHeight;
        }
        else
        {
            DrawText(canvas, "Camera Cuts", 8, y + (RowHeight - 12) / 2, dim: true);
            y += RowHeight;
        }

        // 变换轨行
        foreach (var track in tracks.OfType<TransformTrack>())
        {
            canvas.Children.Add(Rect(NameColumnWidth, y + 2, canvasWidth - NameColumnWidth - 4, RowHeight - 4, RowBrush));
            DrawText(canvas, track.Name, 8, y + (RowHeight - 12) / 2);
            foreach (var key in track.Keys)
            {
                var cx = FrameToX(key.Frame);
                var cy = y + RowHeight / 2;
                canvas.Children.Add(Diamond(cx, cy, 5, KeyBrush));
            }
            y += RowHeight;
        }

        // playhead
        var px = FrameToX(SequenceEditorState.CurrentFrame);
        canvas.Children.Add(Line(px, 0, px, canvas.Height, PlayheadBrush, 2));
    }

    private static void DrawText(Canvas canvas, string text, double x, double y, bool dim = false)
    {
        var brush = dim ? new SolidColorBrush(Color.FromArgb(0x60, 0xC8, 0xD0, 0xD8)) : TextBrush;
        var label = new TextBlock
        {
            Text = text,
            Foreground = brush,
            FontSize = 11,
            Margin = new Thickness(x, y, 0, 0),
        };
        canvas.Children.Add(label);
    }

    private static Avalonia.Controls.Shapes.Polygon Diamond(double cx, double cy, double r, IBrush brush) => new()
    {
        Points = new Points
        {
            new Point(cx, cy - r), new Point(cx + r, cy),
            new Point(cx, cy + r), new Point(cx - r, cy),
        },
        Fill = brush,
    };

    private static Avalonia.Controls.Shapes.Line Line(double x1, double y1, double x2, double y2, IBrush brush, double thickness) => new()
    {
        StartPoint = new Point(x1, y1),
        EndPoint = new Point(x2, y2),
        Stroke = brush,
        StrokeThickness = thickness,
    };

    private static Avalonia.Controls.Shapes.Rectangle Rect(double x, double y, double w, double h, IBrush brush) => new()
    {
        Width = Math.Max(1, w),
        Height = Math.Max(1, h),
        Fill = brush,
        Margin = new Thickness(x, y, 0, 0),
        HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left,
        VerticalAlignment = Avalonia.Layout.VerticalAlignment.Top,
    };

    // ===================================================================
    // 指针交互：拖拽 scrub / 右键删 key、删 shot
    // ===================================================================

    private void OnCanvasPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        var doc = SequenceEditorState.Current;
        if (doc == null) return;
        var point = e.GetCurrentPoint(TimelineCanvas);
        var pos = point.Position;

        if (point.Properties.IsRightButtonPressed)
        {
            if (!TryDeleteKeyAt(doc, pos)) TryDeleteShotAt(doc, pos);
            return;
        }
        if (point.Properties.IsLeftButtonPressed && pos.X >= NameColumnWidth - 6)
        {
            _scrubbing = true;
            ScrubTo(XToFrame(pos.X));
        }
    }

    private void OnCanvasPointerMoved(object? sender, PointerEventArgs e)
    {
        if (!_scrubbing) return;
        var pos = e.GetCurrentPoint(TimelineCanvas).Position;
        ScrubTo(XToFrame(pos.X));
    }

    private void OnCanvasPointerReleased(object? sender, PointerReleasedEventArgs e) => _scrubbing = false;

    private void OnCanvasPointerWheel(object? sender, PointerWheelEventArgs e)
    {
        // 滚轮 = 逐帧步进（按住 Shift 反向）；缩放留二期
        var delta = e.Delta.Y > 0 ? 1 : -1;
        if (e.KeyModifiers.HasFlag(KeyModifiers.Shift)) delta *= 10;
        Seek(SequenceEditorState.CurrentFrame + delta);
        e.Handled = true;
    }

    private void ScrubTo(int frame)
    {
        Seek(frame);
        if (EditorServices.WorldEditor is { } we && SequenceEditorState.Current is { } doc)
            _ = SequenceDriver.ApplyFrame(doc, SequenceEditorState.CurrentFrame, we);
    }

    private void Seek(int frame)
    {
        if (SequenceEditorState.Current == null) return;
        SequenceEditorState.SetFrame(frame);
    }

    /// <summary>命中测试：右键删除关键帧（返回是否命中）。</summary>
    private bool TryDeleteKeyAt(SequenceDocument doc, Point pos)
    {
        var worldEditor = EditorServices.WorldEditor;
        if (worldEditor == null) return false;
        var frame = XToFrame(pos.X);
        double y = RulerHeight + RowHeight; // cut 行之后
        foreach (var track in doc.Tracks.OfType<TransformTrack>())
        {
            var cy = y + RowHeight / 2;
            if (Math.Abs(pos.Y - cy) <= RowHeight / 2)
            {
                var key = track.Keys.FirstOrDefault(k => Math.Abs(FrameToX(k.Frame) - pos.X) <= KeyHitRadius);
                if (key != null)
                {
                    Run(() => _commands.RemoveKey(track.EntityGuid, key.Frame));
                    return true;
                }
            }
            y += RowHeight;
        }
        return false;
    }

    private bool TryDeleteShotAt(SequenceDocument doc, Point pos)
    {
        var cut = doc.CameraCut;
        if (cut == null) return false;
        if (pos.Y < RulerHeight || pos.Y > RulerHeight + RowHeight) return false;
        var frame = XToFrame(pos.X);
        var shot = cut.Shots.FirstOrDefault(s => frame >= s.StartFrame && frame < s.EndFrame);
        if (shot == null) return false;
        var worldEditor = EditorServices.WorldEditor;
        if (worldEditor == null) return false;
        var action = new SequenceUndoActions.RemoveShotAction(cut, shot);
        worldEditor.UndoRedo.Add(action);
        action.Redo();
        SequenceEditorState.FireChanged();
        Logger.Info("[sequencer] 删除分镜头（可 Ctrl+Z）");
        return true;
    }

    private void OnCanvasSizeChanged(object? sender, SizeChangedEventArgs e) => Redraw();

    // ===================================================================
    // 辅助
    // ===================================================================

    private static GameEntity? SelectedEntity() =>
        EditorServices.WorldEditor?.SelectedEntities.FirstOrDefault();

    /// <summary>按钮内部调用命令方法的统一包装：异常 → 编辑器日志（§5.2
    /// UI 按钮 = 对自家命令的内部调用；错误语义与 CLI 完全一致）。</summary>
    private void Run(Action command)
    {
        try
        {
            command();
        }
        catch (CliError error)
        {
            Logger.Warning($"[sequencer] {error.Message}（{error.Hint}）");
        }
        catch (Exception ex)
        {
            Logger.Error($"[sequencer] {ex.Message}", ex);
        }
    }

    private static string Text(TextBox box) =>
        string.IsNullOrWhiteSpace(box.Text) ? "Sequence" : box.Text.Trim();

    private static string Sanitize(string name)
    {
        var invalid = Path.GetInvalidFileNameChars();
        return new string(name.Select(c => invalid.Contains(c) ? '_' : c).ToArray());
    }
}
