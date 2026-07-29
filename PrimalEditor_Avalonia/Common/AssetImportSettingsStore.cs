using System;
using System.IO;
using PrimalEditor_Avalonia.Content;

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
            using var br = new BinaryReader(fs);
            var magic = br.ReadUInt32();
            if (magic != Magic) return t;
            var version = br.ReadUInt32();
            if (version != CurrentVersion) return t; // future version, refuse to misparse
            t.FromBinary(br);
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
        var path = SidecarPath(assetPath);
        var partial = path + ".partial";
        try
        {
            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            using (var fs = File.Create(partial))
            using (var bw = new BinaryWriter(fs))
            {
                bw.Write(Magic);
                bw.Write(CurrentVersion);
                settings.ToBinary(bw);
            }
            if (File.Exists(path)) File.Delete(path);
            File.Move(partial, path);
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"AssetImportSettingsStore: failed to write {path}", ex);
            try { if (File.Exists(partial)) File.Delete(partial); } catch { }
            return false;
        }
    }
}
