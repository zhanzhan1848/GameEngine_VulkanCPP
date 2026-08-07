using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Editors.Texture;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class AssetImportSettingsExtensionsTests
{
    [Fact]
    public void CopyImportSettings_roundtrips_geometry_settings()
    {
        var src = new GeometryImportSettings
        {
            LodMode = LodGenerationMode.Auto,
            Lod0Threshold = 1.5f,
            Lod1Threshold = 51.5f,
            Lod2Threshold = 101.5f,
            GenerateTangentSpace = false,
            RecalculateNormals = true,
            Compression = MeshCompressionMode.Auto,
            ForceSingleLod = true,
            SmoothingAngle = 42.0f,
            CalculateNormals = false,
            CalculateTangents = false,
            ReverseHandedness = true,
            ImportEmbeddedTextures = true,
            ImportAnimations = true,
            CoalesceMeshes = false,
        };

        var dst = new GeometryImportSettings();
        AssetImportSettingsExtensions.CopyImportSettings(src, dst);

        Assert.Equal(src.LodMode, dst.LodMode);
        Assert.Equal(src.SmoothingAngle, dst.SmoothingAngle);
        Assert.Equal(src.CalculateNormals, dst.CalculateNormals);
        Assert.Equal(src.CoalesceMeshes, dst.CoalesceMeshes);
        Assert.Equal(src.ForceSingleLod, dst.ForceSingleLod);
    }

    [Fact]
    public void CopyImportSettings_roundtrips_texture_settings()
    {
        var src = new TextureImportSettings
        {
            Sources = new System.Collections.Generic.List<string> { "/tmp/a.png" },
            Dimension = TextureDimension.TextureCube,
            MipLevels = 7,
            AlphaThreshold = 0.25f,
            PreferBC7 = false,
            OutputFormat = TextureFormat.BC3_UNORM,
            Compress = true,
            IsSRGB = false,
            CompressionMode = TextureCompressionMode.Quality,
            ArraySize = 4,
            Depth = 2,
            IsNormalMap = true,
        };

        var dst = new TextureImportSettings();
        AssetImportSettingsExtensions.CopyImportSettings(src, dst);

        Assert.Equal(src.Dimension, dst.Dimension);
        Assert.Equal(src.MipLevels, dst.MipLevels);
        Assert.Equal(src.AlphaThreshold, dst.AlphaThreshold);
        Assert.Equal(src.OutputFormat, dst.OutputFormat);
        Assert.Equal(src.Compress, dst.Compress);
        Assert.Equal(src.IsNormalMap, dst.IsNormalMap);
    }
}
