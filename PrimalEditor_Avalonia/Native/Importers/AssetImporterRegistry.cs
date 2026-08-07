using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native.Importers;

public static class AssetImporterRegistry
{
    private static readonly List<IAssetImporterPlugin> _plugins = new();

    public static IReadOnlyList<IAssetImporterPlugin> Plugins => _plugins;

    public static void Register(IAssetImporterPlugin plugin)
    {
        if (plugin == null) throw new ArgumentNullException(nameof(plugin));
        if (_plugins.Any(p => p.GetType() == plugin.GetType()))
        {
            Logger.Warning($"AssetImporterRegistry: duplicate plugin {plugin.GetType().Name} ignored");
            return;
        }
        _plugins.Add(plugin);
        Logger.Info($"AssetImporterRegistry: registered {plugin.GetType().Name} ({plugin.DisplayName})");
    }

    public static IAssetImporterPlugin? ResolveForFile(string filePath)
    {
        foreach (var p in _plugins)
            if (p.CanClaim(filePath)) return p;
        return null;
    }

    public static IAssetImporterPlugin? ResolveForType(AssetType type)
    {
        foreach (var p in _plugins)
            foreach (var t in p.HandledTypes)
                if (t == type) return p;
        return null;
    }
}
