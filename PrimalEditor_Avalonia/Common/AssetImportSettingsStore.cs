using System;
using System.IO;
using System.Text.Json;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Metadata;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Sidecar persistence for <see cref="IAssetImportSettings"/>.
///
/// Layout on disk: <c>&lt;asset_path&gt;.meta</c> alongside the asset.
/// File format: <c>&lt;4-byte magic&gt; &lt;4-byte version&gt; &lt;settings body&gt;</c>.
///
/// Magic: <c>PRIM</c> = 0x4D495250 (little-endian read of "PRIM").
/// Version: 1 (current).
/// </summary>
public static class AssetImportSettingsStore
{
    public const uint Magic = 0x4D495250;
    public const uint CurrentVersion = 1;
    public const int MaxSidecarBytes = 4 * 1024 * 1024;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    private static readonly JsonAssetMetaStore JsonStore = new();

    public static string SidecarPath(string assetPath) => assetPath + ".meta";

    /// <summary>
    /// Load settings from <c>assetPath + ".meta"</c>. If the sidecar is missing,
    /// corrupt, or has an incompatible version, returns a default <c>T</c> instance
    /// (no exception) — the user can then save the defaults to seed the sidecar.
    /// </summary>
    public static T Load<T>(string assetPath) where T : IAssetImportSettings, new()
    {
        var t = new T();
        var path = SidecarPath(assetPath);
        if (!File.Exists(path)) return t;
        try
        {
            using var fs = File.OpenRead(path);
            if (fs.Length > MaxSidecarBytes) return t;

            var firstBytes = new byte[8];
            var bytesRead = fs.Read(firstBytes, 0, 8);
            if (bytesRead < 8) return t;

            var magic = BitConverter.ToUInt32(firstBytes, 0);
            if (magic == Magic)
            {
                var version = BitConverter.ToUInt32(firstBytes, 4);
                if (version == CurrentVersion)
                {
                    fs.Position = 0;
                    using var br = new BinaryReader(fs);
                    br.ReadUInt32();
                    br.ReadUInt32();
                    t.FromBinary(br);
                    return t;
                }
            }

            fs.Position = 0;
            using var reader = new StreamReader(fs);
            var json = reader.ReadToEnd();
            var document = JsonSerializer.Deserialize<AssetMetaDocument>(json, JsonOptions);
            if (document?.Settings != null)
            {
                t.FromJson(document.Settings);
            }
            return t;
        }
        catch (Exception ex)
        {
            Logger.Warning($"AssetImportSettingsStore: failed to read {path} — {ex.Message}; using defaults");
            return t;
        }
    }

    /// <summary>
    /// Write settings to <c>assetPath + ".meta"</c>. Atomic via
    /// <c>.partial</c> + rename so a crash mid-write never leaves a
    /// truncated sidecar.
    /// </summary>
    public static bool Save<T>(string assetPath, T settings) where T : IAssetImportSettings
    {
        if (settings == null) return false;

        try
        {
            var document = JsonStore.Read(assetPath);
            var guid = document?.Guid ?? new AssetGuid(Guid.NewGuid());
            document = new AssetMetaDocument
            {
                MetaVersion = AssetMetaDocument.CurrentVersion,
                Guid = guid,
                Importer = GetImporterName<T>(),
                Settings = settings.ToJson()
            };

            return JsonStore.Write(assetPath, document);
        }
        catch (Exception ex)
        {
            Logger.Error($"AssetImportSettingsStore: failed to write {SidecarPath(assetPath)}", ex);
            return false;
        }
    }

    private static string GetImporterName<T>() where T : IAssetImportSettings
    {
        if (typeof(T) == typeof(PrimalEditor_Avalonia.Editors.Texture.TextureImportSettings))
            return "TextureImporter";
        if (typeof(T) == typeof(PrimalEditor_Avalonia.Editors.Geometry.GeometryImportSettings))
            return "GeometryImporter";
        if (typeof(T) == typeof(PrimalEditor_Avalonia.Editors.PrimitiveMesh.PrimitiveInitInfo))
            return "PrimitiveMeshImporter";

        return typeof(T).Name;
    }
}
