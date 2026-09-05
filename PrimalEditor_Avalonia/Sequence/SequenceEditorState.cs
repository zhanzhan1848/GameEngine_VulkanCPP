using System;

namespace PrimalEditor_Avalonia.Sequence;

/// <summary>
/// 当前打开的 Sequence 及播放状态（编辑器面板与 CLI 共享的唯一事实源；
/// 两者都经 UI 线程读写——CliHost 已把全部命令编组到 UI 线程）。
/// 修改类操作走 SequenceUndoActions（undo 归 UndoRedo 栈管），这里只管
/// "打开了什么、当前第几帧、是否在播"这类播放态。
/// </summary>
public static class SequenceEditorState
{
    public static SequenceDocument? Current { get; private set; }

    /// <summary>当前序列的 .pseq 落盘路径（sequence.new 未保存时为 null）。</summary>
    public static string? CurrentPath { get; private set; }

    /// <summary>播放头位置（0..DurationFrames-1）。</summary>
    public static int CurrentFrame { get; private set; }

    /// <summary>预览播放中（DispatcherTimer 驱动，seek/录制期间为 false）。</summary>
    public static bool IsPlaying { get; private set; }

    /// <summary>状态变化通知（面板刷新 playhead/标题等）。</summary>
    public static event Action? Changed;

    public static void FireChanged() => Changed?.Invoke();

    public static bool IsOpen => Current != null;

    public static void Open(SequenceDocument doc, string? path)
    {
        Current = doc ?? throw new ArgumentNullException(nameof(doc));
        CurrentPath = path;
        CurrentFrame = 0;
        IsPlaying = false;
        FireChanged();
    }

    /// <summary>关闭当前序列（返回被关闭的文档；null = 本来就没开）。</summary>
    public static SequenceDocument? Close()
    {
        var closed = Current;
        Current = null;
        CurrentPath = null;
        CurrentFrame = 0;
        IsPlaying = false;
        if (closed != null) FireChanged();
        return closed;
    }

    /// <summary>设置播放头（自动 clamp 到 [0, DurationFrames)）；序列未打开返回 false。</summary>
    public static bool SetFrame(int frame)
    {
        if (Current == null) return false;
        var clamped = Math.Clamp(frame, 0, Math.Max(0, Current.DurationFrames - 1));
        if (clamped != CurrentFrame || frame == clamped) // 首次设置也通知
        {
            CurrentFrame = clamped;
            FireChanged();
            return true;
        }
        return false;
    }

    public static void SetPlaying(bool playing)
    {
        if (Current == null || IsPlaying == playing) return;
        IsPlaying = playing;
        FireChanged();
    }
}
