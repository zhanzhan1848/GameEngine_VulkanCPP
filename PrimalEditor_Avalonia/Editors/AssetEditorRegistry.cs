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
        if (!_factories.TryGetValue(info.Type, out var factory))
        {
            Logger.Warning($"AssetEditorRegistry: no editor for {info.Type} ({info.FileName})");
            return null;
        }
        var vm = factory(info);
        vm.SetAsset(info);
        var host = new AssetEditorHost(vm);
        _openHosts.Add(host);
        host.Closed += (_, _) => _openHosts.Remove(host);
        host.Show();
        Logger.Info($"AssetEditorRegistry: opened {info.Type} editor for {info.FileName}");
        return host;
    }
}