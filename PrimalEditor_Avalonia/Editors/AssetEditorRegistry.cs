using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors;

public static class AssetEditorRegistry
{
    private static readonly Dictionary<AssetType, Func<AssetInfo, AssetEditorViewModel>> _factories = new();
    private static readonly List<AssetEditorHost> _openHosts = new();

    public static ReadOnlyCollection<AssetEditorHost> OpenHosts => _openHosts.AsReadOnly();

    public static void Register(AssetType type, Func<AssetInfo, AssetEditorViewModel> factory)
    {
        _factories[type] = factory;
        Logger.Info($"AssetEditorRegistry: registered editor for {type}");
    }

    public static AssetEditorHost? Open(AssetInfo info)
    {
        // Auto-restore last-known window state. Callers that don't care
        // can ignore the second argument; the registry handles lookup
        // and defaulting.
        var state = LookupPersistedState(info.FullPath);
        return Open(info, state);
    }

    /// <summary>
    /// Look up the persisted window state for <paramref name="assetPath"/>
    /// in the most recently loaded <see cref="EditorLayoutSettings"/>.
    /// Returns null if the layout hasn't been loaded or the path
    /// wasn't in the last session.
    /// </summary>
    public static WindowState? LookupPersistedState(string assetPath)
    {
        var layout = _lastLoadedLayout;
        if (layout == null) return null;
        return layout.AssetWindows.TryGetValue(assetPath, out var s) ? s : null;
    }

    private static EditorLayout? _lastLoadedLayout;

    /// <summary>
    /// Cache the most recently loaded editor layout. Subsequent
    /// <see cref="Open(AssetInfo)"/> calls can use the cached layout
    /// to restore window bounds. Set by <c>MainWindow.OnOpened</c> on
    /// startup; cleared when the editor exits.
    /// </summary>
    public static void SetActiveLayout(EditorLayout layout)
    {
        _lastLoadedLayout = layout;
    }

    public static AssetEditorHost? Open(AssetInfo info, WindowState? initialState)
    {
        if (!_factories.TryGetValue(info.Type, out var factory))
        {
            Logger.Warning($"AssetEditorRegistry: no editor for {info.Type} ({info.FileName})");
            return null;
        }

        foreach (var existing in _openHosts)
        {
            if (existing.ViewModel?.Asset == null) continue;
            if (string.Equals(existing.ViewModel.Asset.FullPath, info.FullPath, StringComparison.OrdinalIgnoreCase))
            {
                existing.Activate();
                Logger.Info($"AssetEditorRegistry: activated existing editor for {info.FileName}");
                return existing;
            }
        }

        var vm = factory(info);
        vm.SetAsset(info);
        var host = new AssetEditorHost(vm, initialState);
        _openHosts.Add(host);
        host.Closed += (_, _) => _openHosts.Remove(host);
        host.Show();
        RecentAssetsManager.Add(info.FullPath);
        Logger.Info($"AssetEditorRegistry: opened {info.Type} editor for {info.FileName}");
        return host;
    }

    public static void Unregister(AssetEditorHost host)
    {
        _openHosts.Remove(host);
    }
}