using System;
using System.Collections.ObjectModel;
using Avalonia.Threading;

namespace PrimalEditor_Avalonia.Common;

public enum LogLevel
{
    Debug,
    Info,
    Warning,
    Error,
}

public class LogEntry
{
    public DateTime Timestamp { get; }
    public LogLevel Level { get; }
    public string Message { get; }
    public string FormattedMessage { get; }

    public LogEntry(DateTime timestamp, LogLevel level, string message)
    {
        Timestamp = timestamp;
        Level = level;
        Message = message;
        FormattedMessage = $"[{timestamp:HH:mm:ss}] [{level.ToString().ToUpper()}] {message}";
    }
}

public class UiLogger : ILogger
{
    public static ObservableCollection<LogEntry> Entries { get; } = new();

    public static event EventHandler<LogEntry>? EntryAdded;

    public void Info(string message) => AddEntry(LogLevel.Info, message);
    public void Warning(string message) => AddEntry(LogLevel.Warning, message);
    public void Error(string message, Exception? ex = null)
    {
        var msg = ex != null ? $"{message}: {ex.Message}" : message;
        AddEntry(LogLevel.Error, msg);
    }
    public void Debug(string message) => AddEntry(LogLevel.Debug, message);

    private static void AddEntry(LogLevel level, string message)
    {
        var entry = new LogEntry(DateTime.Now, level, message);
        // Entries 绑定到 Logger 面板（LoggerViewModel 订阅 CollectionChanged 改
        // 另一个 UI 集合），必须只在 UI 线程变更——Logger 会从任意线程被调用
        // （进程退出回调/引擎日志桥等线程池路径），直接改集合会以未捕获异常
        // 杀死整个编辑器（实测：player 退出事件 → CollectionChanged 重入 → abort）。
        if (Dispatcher.UIThread.CheckAccess())
        {
            Append(entry);
        }
        else
        {
            Dispatcher.UIThread.Post(() => Append(entry));
        }
    }

    private static void Append(LogEntry entry)
    {
        Entries.Add(entry);
        while (Entries.Count > 500)
        {
            Entries.RemoveAt(0);
        }
        EntryAdded?.Invoke(null, entry);
    }

    public static void Clear()
    {
        if (Dispatcher.UIThread.CheckAccess()) Entries.Clear();
        else Dispatcher.UIThread.Post(Entries.Clear);
    }
}