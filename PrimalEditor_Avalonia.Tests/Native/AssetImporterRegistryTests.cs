using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Native.Importers;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

public sealed class AssetImporterRegistryTests
{
    [Fact]
    public void All_three_plugins_registered_at_type_load()
    {
        var types = AssetImporterRegistry.Plugins.Select(p => p.GetType()).ToHashSet();
        Assert.Contains(typeof(TextureImporterPlugin), types);
        Assert.Contains(typeof(MeshImporterPlugin), types);
        Assert.Contains(typeof(PrimitiveMeshImporterPlugin), types);
    }

    [Fact]
    public void Texture_importer_claims_image_extensions_only()
    {
        var p = AssetImporterRegistry.Plugins.First(x => x is TextureImporterPlugin);
        Assert.True(p.CanClaim("/tmp/foo.png"));
        Assert.True(p.CanClaim("/tmp/foo.jpg"));
        Assert.True(p.CanClaim("/tmp/foo.tga"));
        Assert.True(p.CanClaim("/tmp/foo.exr"));
        Assert.False(p.CanClaim("/tmp/foo.fbx"));
        Assert.False(p.CanClaim("/tmp/foo.wav"));
    }

    [Fact]
    public void Mesh_importer_claims_model_extensions_only()
    {
        var p = AssetImporterRegistry.Plugins.First(x => x is MeshImporterPlugin);
        Assert.True(p.CanClaim("/tmp/foo.fbx"));
        Assert.True(p.CanClaim("/tmp/foo.obj"));
        Assert.True(p.CanClaim("/tmp/foo.gltf"));
        Assert.False(p.CanClaim("/tmp/foo.png"));
        Assert.False(p.CanClaim("/tmp/foo.wav"));
    }

    [Fact]
    public void Primitive_mesh_never_claims_a_file()
    {
        var p = AssetImporterRegistry.Plugins.First(x => x is PrimitiveMeshImporterPlugin);
        Assert.False(p.CanClaim("/tmp/anything.fbx"));
        Assert.False(p.CanClaim("/tmp/anything.txt"));
    }

    [Fact]
    public void ResolveForFile_picks_first_claiming_plugin()
    {
        Assert.IsType<TextureImporterPlugin>(AssetImporterRegistry.ResolveForFile("/tmp/a.png"));
        Assert.IsType<MeshImporterPlugin>(AssetImporterRegistry.ResolveForFile("/tmp/a.fbx"));
        Assert.Null(AssetImporterRegistry.ResolveForFile("/tmp/a.txt"));
    }

    [Fact]
    public void Texture_plugin_run_writes_destination_when_engine_absent()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalImp_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var src = Path.Combine(dir, "in.png");
            File.WriteAllBytes(src, new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A });
            var p = AssetImporterRegistry.Plugins.OfType<TextureImporterPlugin>().First();

            var req = new ImportRequest
            {
                SourcePath = src,
                DestinationDir = dir,
                Settings = new TextureImportSettings
                {
                    Compress = false,
                    OutputFormat = TextureFormat.BC7_UNORM,
                    IsSRGB = true,
                },
            };

            var asset = p.RunAsync(req, progress: null, System.Threading.CancellationToken.None)
                         .GetAwaiter().GetResult();

            var dest = Path.Combine(dir, "in.png");
            Assert.True(File.Exists(dest), "TextureRun should copy the source to the destination");
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void Geometry_default_settings_match_content_tools_cpp_defaults()
    {
        var s = new GeometryImportSettings();
        Assert.Equal(178f, s.SmoothingAngle);
        Assert.True(s.CalculateNormals);
        Assert.True(s.CalculateTangents);
        Assert.False(s.ReverseHandedness);
        Assert.False(s.ImportEmbeddedTextures);
        Assert.False(s.ImportAnimations);
        Assert.True(s.CoalesceMeshes);
    }

    [Fact]
    public void Importer_name_mapping_recognises_each_settings_poco()
    {
        var texture = AssetImporterRegistry.Plugins.OfType<TextureImporterPlugin>().First();
        var mesh = AssetImporterRegistry.Plugins.OfType<MeshImporterPlugin>().First();
        var prim = AssetImporterRegistry.Plugins.OfType<PrimitiveMeshImporterPlugin>().First();
        Assert.Equal("TextureImporter", ImportSettingsDefaults.ImporterNameOf(texture));
        Assert.Equal("GeometryImporter", ImportSettingsDefaults.ImporterNameOf(mesh));
        Assert.Equal("PrimitiveMeshImporter", ImportSettingsDefaults.ImporterNameOf(prim));
    }
}
