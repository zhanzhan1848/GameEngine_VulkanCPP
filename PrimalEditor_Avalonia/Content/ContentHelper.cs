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
        ".fbx", ".obj", ".gltf", ".glb", ".dae", ".blend", ".mesh"
    };
    private static readonly string[] AudioExts  = {
        ".wav", ".mp3", ".ogg", ".flac"
    };
    private static readonly string[] ScriptExts = {
        ".py", ".lua", ".cs"
    };
    private static readonly string[] MaterialExts = {
        ".material"
    };
    private static readonly string[] ShaderExts = {
        ".shader"
    };
    private static readonly string[] PrefabExts = {
        ".prefab"
    };

    public static AssetType GetAssetType(string filePath)
    {
        var ext = Path.GetExtension(filePath).ToLowerInvariant();
        if (Array.IndexOf(ImageExts,  ext) >= 0) return AssetType.Texture;
        if (Array.IndexOf(MeshExts,   ext) >= 0) return AssetType.Mesh;
        if (Array.IndexOf(AudioExts,  ext) >= 0) return AssetType.Audio;
        if (Array.IndexOf(ScriptExts, ext) >= 0) return AssetType.Script;
        if (Array.IndexOf(MaterialExts, ext) >= 0) return AssetType.Material;
        if (Array.IndexOf(ShaderExts,  ext) >= 0) return AssetType.Shader;
        if (Array.IndexOf(PrefabExts,  ext) >= 0) return AssetType.Prefab;
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