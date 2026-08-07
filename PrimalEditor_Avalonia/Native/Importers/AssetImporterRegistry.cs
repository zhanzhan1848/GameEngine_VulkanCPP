using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native.Importers;

public static class AssetImporterRegistry
{
    private static readonly List<IAssetImporterPlugin> _plugins = new();
    private static volatile bool _initialized;

    public static IReadOnlyList<IAssetImporterPlugin> Plugins
    {
        get
        {
            EnsureInitialized();
            return _plugins;
        }
    }

    private static void EnsureInitialized()
    {
        if (_initialized) return;
        lock (_plugins)
        {
            if (_initialized) return;
            _initialized = true;
            System.Runtime.CompilerServices.RuntimeHelpers.RunClassConstructor(typeof(TextureImporterPlugin).TypeHandle);
            System.Runtime.CompilerServices.RuntimeHelpers.RunClassConstructor(typeof(MeshImporterPlugin).TypeHandle);
            System.Runtime.CompilerServices.RuntimeHelpers.RunClassConstructor(typeof(PrimitiveMeshImporterPlugin).TypeHandle);
        }
    }

    public static void Register(IAssetImporterPlugin plugin)
    {
        EnsureInitialized();
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
        EnsureInitialized();
        foreach (var p in _plugins)
            if (p.CanClaim(filePath)) return p;
        return null;
    }

    public static IAssetImporterPlugin? ResolveForType(AssetType type)
    {
        EnsureInitialized();
        foreach (var p in _plugins)
            foreach (var t in p.HandledTypes)
                if (t == type) return p;
        return null;
    }
}
