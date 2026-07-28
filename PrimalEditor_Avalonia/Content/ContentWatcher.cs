using System;
using System.IO;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Content;

public class ContentModifiedEventArgs : EventArgs
{
    public string FullPath { get; }
    public WatcherChangeTypes ChangeType { get; }
    public ContentModifiedEventArgs(string path, WatcherChangeTypes t) { FullPath = path; ChangeType = t; }
}

public static class ContentWatcher
{
    private static FileSystemWatcher? _watcher;
    private static System.Timers.Timer? _debounce;
    public static event EventHandler<ContentModifiedEventArgs>? ContentModified;

    public static void Initialize(string root)
    {
        Reset();
        if (!Directory.Exists(root)) return;
        _watcher = new FileSystemWatcher(root)
        {
            IncludeSubdirectories = true,
            NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite,
        };
        _watcher.Created += (_, e) => OnChange(e.FullPath, e.ChangeType);
        _watcher.Changed += (_, e) => OnChange(e.FullPath, e.ChangeType);
        _watcher.Deleted += (_, e) => OnChange(e.FullPath, e.ChangeType);
        _watcher.Renamed += (_, e) => OnChange(e.OldFullPath, WatcherChangeTypes.Renamed);
        _watcher.EnableRaisingEvents = true;

        _debounce = new System.Timers.Timer(250) { AutoReset = false };
        _debounce.Elapsed += (_, _) => Flush();
        Logger.Info($"ContentWatcher watching: {root}");
    }

    private static void OnChange(string path, WatcherChangeTypes type)
    {
        if (_debounce == null) return;
        lock (_debounce)
        {
            _debounce.Stop();
            _debounce.Start();
        }
        ContentModified?.Invoke(null, new ContentModifiedEventArgs(path, type));
    }

    private static void Flush()
    {
        ContentModified?.Invoke(null, new ContentModifiedEventArgs(string.Empty, WatcherChangeTypes.All));
        Logger.Debug("ContentWatcher: flushed debounced change events");
    }

    public static void Reset()
    {
        _watcher?.Dispose(); _watcher = null;
        _debounce?.Dispose(); _debounce = null;
    }
}