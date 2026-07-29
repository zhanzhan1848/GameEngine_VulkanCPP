using System.Collections.Generic;
using System.IO;
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
    }
}
