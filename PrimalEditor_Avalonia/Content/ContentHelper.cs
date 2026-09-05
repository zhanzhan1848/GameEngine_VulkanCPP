using System;
using System.IO;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Content;

public static class ContentHelper
{
    private static readonly string[] ImageExts  = {
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr", ".ktx", ".ktx2",
        ".texture"
    };
    private static readonly string[] MeshExts   = {
        ".fbx", ".obj", ".gltf", ".glb", ".dae", ".blend"
    };
    private static readonly string[] ModelExts  = {
        ".model"
    };

    // ".model.json" is the multi-extension sidecar ContentTools writes
    // next to a cooked .mesh. Path.GetExtension only returns the last
    // suffix (`.json`), so we need a second-hand check by file name.
    private const string ModelSidecarSuffix = ".model.json";
    private static readonly string[] AudioExts  = {
        ".wav", ".mp3", ".ogg", ".flac"
    };
    private static readonly string[] ScriptExts = {
        ".py", ".lua", ".cs"
    };
    private static readonly string[] MaterialExts = {
        ".material"
    };
    // ".material.json" is the multi-extension sidecar MaterialSerializer
    // writes. Path.GetExtension only returns the last suffix (`.json`),
    // so we need a second-hand check by file name — same trick as
    // ModelSidecarSuffix above.
    private const string MaterialSidecarSuffix = ".material.json";
    private static readonly string[] ShaderExts = {
        ".shader"
    };
    private static readonly string[] PrefabExts = {
        ".prefab"
    };
    private static readonly string[] ParticlePresetExts = {
        ".particle.json"
    };
    private static readonly string[] PcgExts = {
        ".pcg"
    };

    // Cooked .mesh files are not user-facing assets; they're managed
    // by the model container and never appear directly in the Content Browser.
    private static readonly string[] HiddenExts = {
        ".mesh"
    };

    public static AssetType GetAssetType(string filePath)
    {
        if (IsDirectory(filePath)) return AssetType.Folder;
        var fileName = Path.GetFileName(filePath).ToLowerInvariant();
        var ext = Path.GetExtension(filePath).ToLowerInvariant();
        if (Array.IndexOf(HiddenExts,  ext) >= 0) return AssetType.Unknown;
        if (Array.IndexOf(ImageExts,  ext) >= 0) return AssetType.Texture;
        if (Array.IndexOf(ModelExts,  ext) >= 0) return AssetType.Model;
        if (fileName.EndsWith(ModelSidecarSuffix, StringComparison.Ordinal)) return AssetType.Model;
        if (Array.IndexOf(MeshExts,   ext) >= 0) return AssetType.Mesh;
        if (Array.IndexOf(AudioExts,  ext) >= 0) return AssetType.Audio;
        if (Array.IndexOf(ScriptExts, ext) >= 0) return AssetType.Script;
        if (Array.IndexOf(MaterialExts, ext) >= 0) return AssetType.Material;
        if (fileName.EndsWith(MaterialSidecarSuffix, StringComparison.Ordinal)) return AssetType.Material;
        if (Array.IndexOf(ShaderExts,  ext) >= 0) return AssetType.Shader;
        if (Array.IndexOf(PrefabExts,  ext) >= 0) return AssetType.Prefab;
        if (fileName.EndsWith(".particle.json", StringComparison.Ordinal)) return AssetType.ParticlePreset;
        if (Array.IndexOf(PcgExts, ext) >= 0) return AssetType.Pcg;
        if (ext == ".pseq") return AssetType.Sequence;
        return AssetType.Unknown;
    }

    public static bool IsDirectory(string path) =>
        Directory.Exists(path) && !File.Exists(path);

    public static bool IsAssetFile(string path) =>
        !IsDirectory(path) && GetAssetType(path) != AssetType.Unknown;

    public static byte[] ComputeHash(string filePath)
    {
        using var sha = System.Security.Cryptography.SHA1.Create();
        using var fs = File.OpenRead(filePath);
        return sha.ComputeHash(fs);
    }
}