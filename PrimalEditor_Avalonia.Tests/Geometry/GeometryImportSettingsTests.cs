using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Native;
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

    [Fact]
    public void AIAssetPipelineSettings_size_matches_expected_ABI_size()
    {
        // Catches field-order or type drift between the C# mirror and the
        // C++ ai_asset_pipeline_params POD in ContentTools.cpp. If this
        // fails, do NOT run the editor — every CookMeshWithPipeline call
        // would silently mis-parse parameters.
        AIAssetPipelineSettings.AssertSize();
    }

    [Fact]
    public void ToPOD_packs_legacy_settings_into_ABI_struct()
    {
        var s = new GeometryImportSettings
        {
            SmoothingAngle = 91.5f,
            CalculateNormals = false,
            CalculateTangents = true,
            ReverseHandedness = true,
            ImportEmbeddedTextures = true,
            ImportAnimations = false,
            CoalesceMeshes = false,
        };

        var pod = s.ToPOD();

        Assert.Equal(91.5f, pod.LegacySmoothingAngle);
        Assert.Equal(0, pod.LegacyCalculateNormals);
        Assert.Equal(1, pod.LegacyCalculateTangents);
        Assert.Equal(1, pod.LegacyReverseHandedness);
        Assert.Equal(1, pod.LegacyImportEmbeddedTextures);
        Assert.Equal(0, pod.LegacyImportAnimations);
        Assert.Equal(0, pod.LegacyCoalesceMeshes);

        // Output fields must be 0 — caller populates them via ToPOD, the
        // native side overwrites them with the actual counts after the cook.
        Assert.Equal(0u, pod.MeshCount);
        Assert.Equal(0u, pod.MeshletCount);
        Assert.Equal(0u, pod.ValidationErrors);
        Assert.Equal(0u, pod.ValidationWarnings);
    }

    [Fact]
    public void ToPOD_packs_pipeline_settings_into_ABI_struct()
    {
        var s = new GeometryImportSettings
        {
            EnableRepair = true,
            EnableRemesh = false,
            EnableSubdivide = true,
            EnableUvatlas = true,
            EnableLod = true,
            EnableMeshlet = true,
            EnableCollision = false,
            EnableDerive = true,
            AutoDeriveAfterSubdivide = false,
            EnableSdf = true,
            SubdivideScheme = PipelineSubdivideScheme.CatmullClark,
            SubdivideLevels = 3,
            DeriveNormalMode = PipelineDeriveNormalMode.Smooth,
            DeriveTangentMode = PipelineDeriveTangentMode.None,
            DeriveFacetedAngleDeg = 45f,
            LodRatio = 0.4f,
            LodMaxLevels = 4,
            MinVerticesForLod = 1000,
            MeshletMaxVertices = 192,
            MeshletMaxTriangles = 96,
            CollisionMaxHulls = 16,
        };

        var pod = s.ToPOD();

        Assert.Equal(1, pod.EnableRepair);
        Assert.Equal(0, pod.EnableRemesh);
        Assert.Equal(1, pod.EnableSubdivide);
        Assert.Equal(1, pod.EnableUvatlas);
        Assert.Equal(1, pod.EnableLod);
        Assert.Equal(1, pod.EnableMeshlet);
        Assert.Equal(0, pod.EnableCollision);
        Assert.Equal(1, pod.EnableDerive);
        Assert.Equal(0, pod.AutoDeriveAfterSubdivide);
        Assert.Equal(1, pod.EnableSdf);

        Assert.Equal((byte)PipelineSubdivideScheme.CatmullClark, pod.SubdivideScheme);
        Assert.Equal(3u, pod.SubdivideLevels);
        Assert.Equal((byte)PipelineDeriveNormalMode.Smooth, pod.DeriveNormalMode);
        Assert.Equal((byte)PipelineDeriveTangentMode.None, pod.DeriveTangentMode);
        Assert.Equal(45f, pod.DeriveFacetedAngleDeg);

        Assert.Equal(0.4f, pod.LodRatio);
        Assert.Equal(4u, pod.LodMaxLevels);
        Assert.Equal(1000u, pod.MinVerticesForLod);
        Assert.Equal(192u, pod.MeshletMaxVertices);
        Assert.Equal(96u, pod.MeshletMaxTriangles);
        Assert.Equal(16u, pod.CollisionMaxHulls);
    }
}
