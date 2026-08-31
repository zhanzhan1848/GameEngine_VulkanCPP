using System;
using System.Collections.ObjectModel;

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
        Entries.Add(entry);
        while (Entries.Count > 500)
        {
            Entries.RemoveAt(0);
        }
        EntryAdded?.Invoke(null, entry);
    }

    public static void Clear() => Entries.Clear();
}