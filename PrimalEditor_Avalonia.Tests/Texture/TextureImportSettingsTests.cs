using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Editors.Texture;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Texture;

public sealed class TextureImportSettingsTests
{
    [Fact]
    public void Defaults_IsNormalMapIsFalse()
    {
        var s = new TextureImportSettings();
        Assert.False(s.IsNormalMap);
        Assert.True(s.IsSRGB);
    }

    [Fact]
    public void BinaryRoundTrip_PreservesAllFields()
    {
        var s = new TextureImportSettings
        {
            Sources = new System.Collections.Generic.List<string> { "a.png", "b.png" },
            Dimension = TextureDimension.TextureCube,
            MipLevels = 9,
            AlphaThreshold = 0.25f,
            PreferBC7 = false,
            OutputFormat = TextureFormat.BC5_UNORM,
            Compress = false,
            IsSRGB = false,
            CompressionMode = TextureCompressionMode.Quality,
            ArraySize = 4,
            Depth = 2,
            IsNormalMap = true,
        };

        using var ms = new MemoryStream();
        using (var w = new BinaryWriter(ms, System.Text.Encoding.UTF8, leaveOpen: true))
        {
            s.ToBinary(w);
        }
        ms.Position = 0;
        var loaded = new TextureImportSettings();
        using (var r = new BinaryReader(ms, System.Text.Encoding.UTF8, leaveOpen: true))
        {
            loaded.FromBinary(r);
        }

        Assert.Equal(s.Sources, loaded.Sources);
        Assert.Equal(s.Dimension, loaded.Dimension);
        Assert.Equal(s.MipLevels, loaded.MipLevels);
        Assert.Equal(s.AlphaThreshold, loaded.AlphaThreshold);
        Assert.Equal(s.PreferBC7, loaded.PreferBC7);
        Assert.Equal(s.OutputFormat, loaded.OutputFormat);
        Assert.Equal(s.Compress, loaded.Compress);
        Assert.Equal(s.IsSRGB, loaded.IsSRGB);
        Assert.Equal(s.CompressionMode, loaded.CompressionMode);
        Assert.Equal(s.ArraySize, loaded.ArraySize);
        Assert.Equal(s.Depth, loaded.Depth);
        Assert.Equal(s.IsNormalMap, loaded.IsNormalMap);
    }

    [Fact]
    public void BinaryBackwardCompat_LoadsOldSidecarWithoutIsNormalMap()
    {
        // Build a "v1 pre-IsNormalMap" sidecar: write all fields but stop before IsNormalMap.
        using var ms = new MemoryStream();
        using (var w = new BinaryWriter(ms, System.Text.Encoding.UTF8, leaveOpen: true))
        {
            var old = new TextureImportSettings
            {
                Sources = new System.Collections.Generic.List<string> { "old.png" },
                Dimension = TextureDimension.Texture2D,
                MipLevels = 1,
                AlphaThreshold = 0.5f,
                PreferBC7 = true,
                OutputFormat = TextureFormat.BC7_UNORM,
                Compress = true,
                IsSRGB = true,
                CompressionMode = TextureCompressionMode.Fast,
                ArraySize = 1,
                Depth = 1,
            };
            // Replicate the pre-fix writer by writing exactly the bytes that
            // would have been written before IsNormalMap was introduced.
            w.Write(old.Sources.Count);
            foreach (var src in old.Sources) w.Write(src);
            w.Write((int)old.Dimension);
            w.Write(old.MipLevels);
            w.Write(old.AlphaThreshold);
            w.Write(old.PreferBC7);
            w.Write((int)old.OutputFormat);
            w.Write(old.Compress);
            w.Write(old.IsSRGB);
            w.Write((int)old.CompressionMode);
            w.Write(old.ArraySize);
            w.Write(old.Depth);
        }
        ms.Position = 0;
        var loaded = new TextureImportSettings();
        using (var r = new BinaryReader(ms, System.Text.Encoding.UTF8, leaveOpen: true))
        {
            loaded.FromBinary(r);
        }

        Assert.True(loaded.IsSRGB);
        Assert.False(loaded.IsNormalMap);
        Assert.Equal(TextureFormat.BC7_UNORM, loaded.OutputFormat);
    }
}
