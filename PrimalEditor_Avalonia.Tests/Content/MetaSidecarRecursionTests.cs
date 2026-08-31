using System.IO;
using System.Text.Json.Nodes;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Metadata;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

/// <summary>
/// Regression: importing <c>foo.fbx</c> writes <c>foo.fbx.meta</c>;
/// a FileSystemWatcher Created event for that .meta file used to be
/// fed back into AssetRegistry.Register, which wrote
/// <c>foo.fbx.meta.meta</c>, firing yet another watcher event,
/// eventually exceeding the OS path-length limit and freezing the
/// editor.
/// </summary>
public sealed class MetaSidecarRecursionTests
{
    [Fact]
    public void JsonAssetMetaStore_Write_RejectsMetaSuffixedAssetPath()
    {
        using var tempDir = new TemporaryDirectory();
        var metaPath = Path.Combine(tempDir.Path, "foo.fbx.meta");

        var store = new JsonAssetMetaStore();
        var doc = new AssetMetaDocument
        {
            MetaVersion = 1,
            Guid = new PrimalEditor_Avalonia.Content.AssetGuid(System.Guid.NewGuid()),
            Importer = "GeometryImporter",
            Settings = new JsonObject(),
        };

        var result = store.Write(metaPath, doc);

        Assert.False(result);
        Assert.False(File.Exists(metaPath + ".meta"));
        Assert.False(File.Exists(metaPath + ".partial"));
    }

    [Fact]
    public void AssetRegistry_Register_IgnoresMetaSuffixedPaths()
    {
        using var tempDir = new TemporaryDirectory();
        var metaPath = Path.Combine(tempDir.Path, "foo.fbx.meta");
        File.WriteAllText(metaPath, "{}");

        try
        {
            AssetRegistry.Initialize(tempDir.Path);
            var info = AssetRegistry.Register(metaPath);

            Assert.Null(info);
            Assert.False(File.Exists(metaPath + ".meta"));
        }
        finally
        {
            AssetRegistry.Reset();
        }
    }

    [Fact]
    public void AssetRegistry_Register_IgnoresDeeplyNestedMetaPaths()
    {
        using var tempDir = new TemporaryDirectory();
        var nestedMeta = Path.Combine(tempDir.Path, "foo.fbx.meta.meta");
        File.WriteAllText(nestedMeta, "{}");

        try
        {
            AssetRegistry.Initialize(tempDir.Path);
            var info = AssetRegistry.Register(nestedMeta);

            Assert.Null(info);
            Assert.False(File.Exists(nestedMeta + ".meta"));
        }
        finally
        {
            AssetRegistry.Reset();
        }
    }

    [Fact]
    public void ContentWatcherMetaEvent_DoesNotRegisterMetaSidecar()
    {
        using var tempDir = new TemporaryDirectory();
        var metaPath = Path.Combine(tempDir.Path, "foo.fbx.meta");
        File.WriteAllText(metaPath, "{}");

        try
        {
            AssetRegistry.Initialize(tempDir.Path);
            AssetRegistry.Register(metaPath);

            Assert.Empty(AssetRegistry.Assets);
        }
        finally
        {
            AssetRegistry.Reset();
        }
    }
}
