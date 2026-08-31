using System;
using System.IO;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Content;

/// <summary>
/// Describes a single file-system change observed by
/// <see cref="ContentWatcher"/>. The Old/New paths are populated as
/// follows:
///
/// <list type="bullet">
///   <item>Created: OldPath = null, NewPath = created path</item>
///   <item>Deleted: OldPath = deleted path, NewPath = null</item>
///   <item>Changed: OldPath = null, NewPath = modified path</item>
///   <item>Renamed: OldPath = original, NewPath = new; IsMove = true
///     when source and destination share a parent directory (intra-dir
///     move), false for cross-directory rename.</item>
/// </list>
///
/// Consumers should treat the OldPath/NewPath pair atomically — the
/// debouncer coalesces many file events into one notification, so
/// between debounce flushes, OldPath may be null even when a delete
/// was rolled up.
/// </summary>
public class ContentModifiedEventArgs : EventArgs
{
    public string? OldPath { get; }
    public string? NewPath { get; }
    public WatcherChangeTypes ChangeType { get; }
    public bool IsMove { get; }
    public ContentModifiedEventArgs(string? oldPath, string? newPath, WatcherChangeTypes type, bool isMove = false)
    {
        OldPath = oldPath;
        NewPath = newPath;
        ChangeType = type;
        IsMove = isMove;
    }
}

/// <summary>
/// Watches the project's Content directory for file-system changes
/// and forwards them to <see cref="ContentModified"/> subscribers as
/// <see cref="ContentModifiedEventArgs"/>. Internally coalesces
/// bursts (rapid saves) into a single debounced flush, but always
/// preserves the OldPath/NewPath pair so the consumer can
/// distinguish create/delete/rename unambiguously.
/// </summary>
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
        _watcher.Created += (_, e) => OnChange(new ContentModifiedEventArgs(null, e.FullPath, e.ChangeType));
        _watcher.Changed += (_, e) => OnChange(new ContentModifiedEventArgs(null, e.FullPath, e.ChangeType));
        _watcher.Deleted += (_, e) => OnChange(new ContentModifiedEventArgs(e.FullPath, null, e.ChangeType));
        _watcher.Renamed += (_, e) => OnChange(BuildRenameEvent(e));
        _watcher.EnableRaisingEvents = true;

        _debounce = new System.Timers.Timer(250) { AutoReset = false };
        _debounce.Elapsed += (_, _) => Flush();
        Logger.Info($"ContentWatcher watching: {root}");
    }

    private static ContentModifiedEventArgs BuildRenameEvent(RenamedEventArgs e)
    {
        bool isMove = !string.IsNullOrEmpty(e.OldFullPath)
            && !string.IsNullOrEmpty(e.FullPath)
            && string.Equals(
                System.IO.Path.GetDirectoryName(e.OldFullPath),
                System.IO.Path.GetDirectoryName(e.FullPath),
                StringComparison.OrdinalIgnoreCase);
        return new ContentModifiedEventArgs(e.OldFullPath, e.FullPath, WatcherChangeTypes.Renamed, isMove);
    }

    private static void OnChange(ContentModifiedEventArgs args)
    {
        if (_debounce == null) return;
        lock (_debounce)
        {
            _debounce.Stop();
            _debounce.Start();
        }
        ContentModified?.Invoke(null, args);
    }

    /// <summary>
    /// Force a debounced flush. Subscribers that rely on event order
    /// (Create → Modify → Rename) should call this on shutdown.
    /// </summary>
    public static void Flush() => ContentModified?.Invoke(null,
        new ContentModifiedEventArgs(null, null, WatcherChangeTypes.All));

    public static void Reset()
    {
        _watcher?.Dispose(); _watcher = null;
        _debounce?.Dispose(); _debounce = null;
    }
}
