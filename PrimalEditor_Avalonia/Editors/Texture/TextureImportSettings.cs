using System.Collections.Generic;
using System.IO;
using System.Text.Json.Nodes;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Editors.Texture;

public enum TextureDimension { Texture2D, TextureCube, Texture3D }
public enum TextureFormat { R8G8B8A8_UNORM, BC7_UNORM, BC1_UNORM, BC3_UNORM, BC5_UNORM }
public enum TextureCompressionMode { None, Fast, Quality }

public class TextureImportSettings : IAssetImportSettings
{
    public List<string> Sources { get; set; } = new();
    public TextureDimension Dimension { get; set; } = TextureDimension.Texture2D;
    public int MipLevels { get; set; } = 0;
    public float AlphaThreshold { get; set; } = 0.5f;
    public bool PreferBC7 { get; set; } = true;
    public TextureFormat OutputFormat { get; set; } = TextureFormat.BC7_UNORM;
    public bool Compress { get; set; } = true;
    public bool IsSRGB { get; set; } = true;
    public TextureCompressionMode CompressionMode { get; set; } = TextureCompressionMode.Fast;
    public int ArraySize { get; set; } = 1;
    public int Depth { get; set; } = 1;
    public bool IsNormalMap { get; set; } = false;

    public void ToBinary(BinaryWriter w)
    {
        w.Write(Sources.Count);
        foreach (var s in Sources) w.Write(s);
        w.Write((int)Dimension);
        w.Write(MipLevels);
        w.Write(AlphaThreshold);
        w.Write(PreferBC7);
        w.Write((int)OutputFormat);
        w.Write(Compress);
        w.Write(IsSRGB);
        w.Write((int)CompressionMode);
        w.Write(ArraySize);
        w.Write(Depth);
        w.Write(IsNormalMap);
    }

    public void FromBinary(BinaryReader r)
    {
        var n = r.ReadInt32();
        Sources.Clear();
        for (var i = 0; i < n; ++i)
        {
            var s = r.ReadString();
            if (s != null) Sources.Add(s);
        }
        Dimension = (TextureDimension)r.ReadInt32();
        MipLevels = r.ReadInt32();
        AlphaThreshold = r.ReadSingle();
        PreferBC7 = r.ReadBoolean();
        OutputFormat = (TextureFormat)r.ReadInt32();
        Compress = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length)
            IsSRGB = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length)
            CompressionMode = (TextureCompressionMode)r.ReadInt32();
        if (r.BaseStream.Position < r.BaseStream.Length)
            ArraySize = r.ReadInt32();
        if (r.BaseStream.Position < r.BaseStream.Length)
            Depth = r.ReadInt32();
        if (r.BaseStream.Position < r.BaseStream.Length)
            IsNormalMap = r.ReadBoolean();
    }

    public void FromJson(JsonObject settings)
    {
        if (settings.TryGetPropertyValue("Sources", out var sourcesNode) && sourcesNode is JsonArray sourcesArray)
        {
            Sources.Clear();
            foreach (var item in sourcesArray)
            {
                if (item is JsonValue value && value.TryGetValue(out string? source))
                {
                    Sources.Add(source!);
                }
            }
        }

        if (settings.TryGetPropertyValue("Dimension", out var dimensionNode) && dimensionNode is JsonValue dimensionValue)
        {
            if (dimensionValue.TryGetValue(out int dimensionInt))
            {
                Dimension = (TextureDimension)dimensionInt;
            }
        }

        if (settings.TryGetPropertyValue("MipLevels", out var mipLevelsNode) && mipLevelsNode is JsonValue mipLevelsValue)
        {
            if (mipLevelsValue.TryGetValue(out int mipLevels))
            {
                MipLevels = mipLevels;
            }
        }

        if (settings.TryGetPropertyValue("AlphaThreshold", out var alphaThresholdNode) && alphaThresholdNode is JsonValue alphaThresholdValue)
        {
            if (alphaThresholdValue.TryGetValue(out float alphaThreshold))
            {
                AlphaThreshold = alphaThreshold;
            }
        }

        if (settings.TryGetPropertyValue("PreferBC7", out var preferBC7Node) && preferBC7Node is JsonValue preferBC7Value)
        {
            if (preferBC7Value.TryGetValue(out bool preferBC7))
            {
                PreferBC7 = preferBC7;
            }
        }

        if (settings.TryGetPropertyValue("OutputFormat", out var outputFormatNode) && outputFormatNode is JsonValue outputFormatValue)
        {
            if (outputFormatValue.TryGetValue(out int outputFormatInt))
            {
                OutputFormat = (TextureFormat)outputFormatInt;
            }
        }

        if (settings.TryGetPropertyValue("Compress", out var compressNode) && compressNode is JsonValue compressValue)
        {
            if (compressValue.TryGetValue(out bool compress))
            {
                Compress = compress;
            }
        }

        if (settings.TryGetPropertyValue("IsSRGB", out var isSRGBNode) && isSRGBNode is JsonValue isSRGBValue)
        {
            if (isSRGBValue.TryGetValue(out bool isSRGB))
            {
                IsSRGB = isSRGB;
            }
        }

        if (settings.TryGetPropertyValue("CompressionMode", out var compressionModeNode) && compressionModeNode is JsonValue compressionModeValue)
        {
            if (compressionModeValue.TryGetValue(out int compressionModeInt))
            {
                CompressionMode = (TextureCompressionMode)compressionModeInt;
            }
        }

        if (settings.TryGetPropertyValue("ArraySize", out var arraySizeNode) && arraySizeNode is JsonValue arraySizeValue)
        {
            if (arraySizeValue.TryGetValue(out int arraySize))
            {
                ArraySize = arraySize;
            }
        }

        if (settings.TryGetPropertyValue("Depth", out var depthNode) && depthNode is JsonValue depthValue)
        {
            if (depthValue.TryGetValue(out int depth))
            {
                Depth = depth;
            }
        }

        if (settings.TryGetPropertyValue("IsNormalMap", out var isNormalMapNode) && isNormalMapNode is JsonValue isNormalMapValue)
        {
            if (isNormalMapValue.TryGetValue(out bool isNormalMap))
            {
                IsNormalMap = isNormalMap;
            }
        }
    }

    public JsonObject ToJson()
    {
        var sourcesArray = new JsonArray();
        foreach (var source in Sources)
        {
            sourcesArray.Add(source);
        }

        return new JsonObject
        {
            ["Sources"] = sourcesArray,
            ["Dimension"] = (int)Dimension,
            ["MipLevels"] = MipLevels,
            ["AlphaThreshold"] = AlphaThreshold,
            ["PreferBC7"] = PreferBC7,
            ["OutputFormat"] = (int)OutputFormat,
            ["Compress"] = Compress,
            ["IsSRGB"] = IsSRGB,
            ["CompressionMode"] = (int)CompressionMode,
            ["ArraySize"] = ArraySize,
            ["Depth"] = Depth,
            ["IsNormalMap"] = IsNormalMap
        };
    }
}
