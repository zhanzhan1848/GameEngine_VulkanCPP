using System.IO;
using System.Threading;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.Importers;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

/// <summary>
/// Contract: importing a model source file (.fbx / .obj / .gltf / ...)
/// must NOT copy the source into the project directory. Everything the
/// import produces (cooked .mesh, .model.json sidecar, default material,
/// cooked .texture assets) lives under a per-model asset folder
/// "&lt;Content&gt;/&lt;stem&gt;/"; the source stays at its original
/// location on disk. This is the regression guard for the no-copy import
/// feature and the per-model folder layout.
/// </summary>
public sealed class MeshImporterNoCopyTests
{
    [Fact]
    public async System.Threading.Tasks.Task RunAsync_WhenEngineNotLoaded_DoesNotCopySourceFile()
    {
        using var temp = new TemporaryProject();
        var externalDir = Path.Combine(temp.Path, "_external_sources");
        Directory.CreateDirectory(externalDir);
        var sourcePath = Path.Combine(externalDir, "monster.fbx");
        var sourceBytes = new byte[] { 0x66, 0x61, 0x6B, 0x65, 0x2D, 0x66, 0x62, 0x78 };
        File.WriteAllBytes(sourcePath, sourceBytes);
        var sourceMtime = File.GetLastWriteTimeUtc(sourcePath);

        var plugin = new MeshImporterPlugin();
        var request = new ImportRequest
        {
            SourcePath = sourcePath,
            DestinationDir = temp.ContentPath,
            Settings = new GeometryImportSettings(),
        };

        var result = await plugin.RunAsync(request, progress: null, CancellationToken.None);

        // No engine in the test environment, so the cook fails. The
        // assertion of interest is the side-effect contract: the source
        // must remain untouched and no copy must exist in the project.
        Assert.False(result.Success);
        Assert.Equal(AssetType.Model, result.Type);
        Assert.Equal(sourcePath, result.SourcePath);
        Assert.Equal(Path.Combine(temp.ContentPath, "monster", "monster.model.json"), result.DestinationPath);

        Assert.True(File.Exists(sourcePath), "source must remain on disk");
        Assert.Equal(sourceBytes, File.ReadAllBytes(sourcePath));
        Assert.Equal(sourceMtime, File.GetLastWriteTimeUtc(sourcePath));

        var copiedSource = Path.Combine(temp.ContentPath, "monster.fbx");
        Assert.False(File.Exists(copiedSource), "source must NOT be copied into the project");

        // Engine-unavailable failure must not leave artifacts (or even the
        // asset folder) behind.
        Assert.False(Directory.Exists(Path.Combine(temp.ContentPath, "monster")),
            "failed import must not create the per-model asset folder");

        var assetDir = Path.Combine(temp.ContentPath, "monster");
        Assert.False(File.Exists(Path.Combine(assetDir, "monster.mesh")));
        Assert.False(File.Exists(Path.Combine(assetDir, "monster.model.json")));
    }
}
