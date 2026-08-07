using System.IO;
using System.Text.Json.Nodes;
using PrimalEditor_Avalonia.Editors.Geometry;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Geometry;

public sealed class GeometryImportSettingsTests
{
    [Fact]
    public void Default_constructor_uses_expected_defaults()
    {
        var s = new GeometryImportSettings();

        Assert.Equal(LodGenerationMode.Manual, s.LodMode);
        Assert.Equal(0f, s.Lod0Threshold);
        Assert.Equal(50f, s.Lod1Threshold);
        Assert.Equal(100f, s.Lod2Threshold);
        Assert.True(s.GenerateTangentSpace);
        Assert.False(s.RecalculateNormals);
        Assert.Equal(MeshCompressionMode.None, s.Compression);
        Assert.False(s.ForceSingleLod);

        Assert.Equal(178f, s.SmoothingAngle);
        Assert.True(s.CalculateNormals);
        Assert.True(s.CalculateTangents);
        Assert.False(s.ReverseHandedness);
        Assert.False(s.ImportEmbeddedTextures);
        Assert.False(s.ImportAnimations);
        Assert.True(s.CoalesceMeshes);
    }

    [Fact]
    public void ToBinary_FromBinary_roundtrip_all_fields()
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

        byte[] payload;
        using (var ms = new MemoryStream())
        {
            using (var w = new BinaryWriter(ms)) { src.ToBinary(w); }
            payload = ms.ToArray();
        }

        var dst = new GeometryImportSettings();
        using (var ms = new MemoryStream(payload))
        using (var r = new BinaryReader(ms)) { dst.FromBinary(r); }

        Assert.Equal(src.LodMode, dst.LodMode);
        Assert.Equal(src.Lod0Threshold, dst.Lod0Threshold);
        Assert.Equal(src.Lod1Threshold, dst.Lod1Threshold);
        Assert.Equal(src.Lod2Threshold, dst.Lod2Threshold);
        Assert.Equal(src.GenerateTangentSpace, dst.GenerateTangentSpace);
        Assert.Equal(src.RecalculateNormals, dst.RecalculateNormals);
        Assert.Equal(src.Compression, dst.Compression);
        Assert.Equal(src.ForceSingleLod, dst.ForceSingleLod);
        Assert.Equal(src.SmoothingAngle, dst.SmoothingAngle);
        Assert.Equal(src.CalculateNormals, dst.CalculateNormals);
        Assert.Equal(src.CalculateTangents, dst.CalculateTangents);
        Assert.Equal(src.ReverseHandedness, dst.ReverseHandedness);
        Assert.Equal(src.ImportEmbeddedTextures, dst.ImportEmbeddedTextures);
        Assert.Equal(src.ImportAnimations, dst.ImportAnimations);
        Assert.Equal(src.CoalesceMeshes, dst.CoalesceMeshes);
    }

    [Fact]
    public void ToJson_FromJson_roundtrip_all_fields()
    {
        var src = new GeometryImportSettings
        {
            LodMode = LodGenerationMode.Auto,
            Lod0Threshold = 2.5f,
            Lod1Threshold = 52.5f,
            Lod2Threshold = 102.5f,
            GenerateTangentSpace = false,
            RecalculateNormals = true,
            Compression = MeshCompressionMode.Auto,
            ForceSingleLod = true,
            SmoothingAngle = 33.0f,
            CalculateNormals = false,
            CalculateTangents = false,
            ReverseHandedness = true,
            ImportEmbeddedTextures = true,
            ImportAnimations = true,
            CoalesceMeshes = false,
        };

        JsonObject json = src.ToJson();
        var dst = new GeometryImportSettings();
        dst.FromJson(json);

        Assert.Equal(src.LodMode, dst.LodMode);
        Assert.Equal(src.Lod0Threshold, dst.Lod0Threshold);
        Assert.Equal(src.Lod1Threshold, dst.Lod1Threshold);
        Assert.Equal(src.Lod2Threshold, dst.Lod2Threshold);
        Assert.Equal(src.GenerateTangentSpace, dst.GenerateTangentSpace);
        Assert.Equal(src.RecalculateNormals, dst.RecalculateNormals);
        Assert.Equal(src.Compression, dst.Compression);
        Assert.Equal(src.ForceSingleLod, dst.ForceSingleLod);
        Assert.Equal(src.SmoothingAngle, dst.SmoothingAngle);
        Assert.Equal(src.CalculateNormals, dst.CalculateNormals);
        Assert.Equal(src.CalculateTangents, dst.CalculateTangents);
        Assert.Equal(src.ReverseHandedness, dst.ReverseHandedness);
        Assert.Equal(src.ImportEmbeddedTextures, dst.ImportEmbeddedTextures);
        Assert.Equal(src.ImportAnimations, dst.ImportAnimations);
        Assert.Equal(src.CoalesceMeshes, dst.CoalesceMeshes);
    }

    [Fact]
    public void FromBinary_legacy_short_blob_uses_defaults_for_new_fields()
    {
        // 8 writes matching the pre-extension ToBinary order (LodMode..ForceSingleLod).
        byte[] payload;
        using (var ms = new MemoryStream())
        {
            using (var w = new BinaryWriter(ms))
            {
                w.Write((int)LodGenerationMode.Auto);
                w.Write(2.5f);
                w.Write(52.5f);
                w.Write(102.5f);
                w.Write(true);
                w.Write(false);
                w.Write((int)MeshCompressionMode.None);
                w.Write(false);
            }
            payload = ms.ToArray();
        }

        var dst = new GeometryImportSettings();
        using (var ms = new MemoryStream(payload))
        using (var r = new BinaryReader(ms)) { dst.FromBinary(r); }

        Assert.Equal(LodGenerationMode.Auto, dst.LodMode);
        Assert.Equal(2.5f, dst.Lod0Threshold);
        Assert.Equal(52.5f, dst.Lod1Threshold);
        Assert.Equal(102.5f, dst.Lod2Threshold);
        Assert.True(dst.GenerateTangentSpace);
        Assert.False(dst.RecalculateNormals);
        Assert.Equal(MeshCompressionMode.None, dst.Compression);
        Assert.False(dst.ForceSingleLod);

        Assert.Equal(178f, dst.SmoothingAngle);
        Assert.True(dst.CalculateNormals);
        Assert.True(dst.CalculateTangents);
        Assert.False(dst.ReverseHandedness);
        Assert.False(dst.ImportEmbeddedTextures);
        Assert.False(dst.ImportAnimations);
        Assert.True(dst.CoalesceMeshes);
    }
}
