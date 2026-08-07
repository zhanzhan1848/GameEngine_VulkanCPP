using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

/// <summary>
/// Regression: importing a .fbx writes <c>foo.fbx</c>, <c>foo.mesh</c>,
/// and <c>foo.fbx.meta</c> into Content/Assets. The Content Browser
/// must list the visible assets (.fbx, .mesh) after the import flow.
/// </summary>
public sealed class ContentBrowserDisplayRegressionTests
{
    [Fact]
    public void RegisterFbx_ShowsUpInAssetRegistry_AfterImportSideEffects()
    {
        using var tempDir = new TemporaryDirectory();
        var contentRoot = Path.Combine(tempDir.Path, "Content");
        Directory.CreateDirectory(contentRoot);
        var assetsDir = Path.Combine(contentRoot, "Assets");
        Directory.CreateDirectory(assetsDir);

        var fbxPath = Path.Combine(assetsDir, "corridor.fbx");
        File.WriteAllText(fbxPath, "fake-fbx");
        var meshPath = Path.Combine(assetsDir, "corridor.mesh");
        File.WriteAllText(meshPath, "fake-mesh");

        try
        {
            AssetRegistry.Initialize(contentRoot);

            Assert.Contains(AssetRegistry.Assets, a => a.FullPath == fbxPath);
            Assert.Contains(AssetRegistry.Assets, a => a.FullPath == meshPath);
        }
        finally
        {
            AssetRegistry.Reset();
        }
    }

    [Fact]
    public void ContentBrowser_FilesCollection_PopulatesFromRealImport()
    {
        using var tempDir = new TemporaryDirectory();
        var contentRoot = Path.Combine(tempDir.Path, "Content");
        Directory.CreateDirectory(contentRoot);
        var assetsDir = Path.Combine(contentRoot, "Assets");
        Directory.CreateDirectory(assetsDir);

        File.WriteAllText(Path.Combine(assetsDir, "corridor.fbx"), "fake-fbx");
        File.WriteAllText(Path.Combine(assetsDir, "corridor.mesh"), "fake-mesh");

        try
        {
            AssetRegistry.Initialize(contentRoot);
            var files = Directory.GetFiles(assetsDir)
                .Select(AssetRegistry.Register)
                .Where(i => i != null && i.Type != AssetType.Unknown)
                .ToList();

            Assert.Equal(2, files.Count);
        }
        finally
        {
            AssetRegistry.Reset();
        }
    }

    [Fact]
    public void ContentBrowserFiles_PopulateAfterWatcherDrivenRegister()
    {
        using var tempDir = new TemporaryDirectory();
        var contentRoot = Path.Combine(tempDir.Path, "Content");
        Directory.CreateDirectory(contentRoot);
        var assetsDir = Path.Combine(contentRoot, "Assets");
        Directory.CreateDirectory(assetsDir);

        File.WriteAllText(Path.Combine(assetsDir, "corridor.fbx"), "fake-fbx");
        File.WriteAllText(Path.Combine(assetsDir, "corridor.mesh"), "fake-mesh");
        File.WriteAllText(Path.Combine(assetsDir, "corridor.fbx.meta"), "{}");

        try
        {
            AssetRegistry.Initialize(contentRoot);

            foreach (var file in Directory.GetFiles(assetsDir))
            {
                AssetRegistry.Register(file);
            }

            var visible = Directory.GetFiles(assetsDir)
                .Where(f => !Path.GetFileName(f).EndsWith(".meta"))
                .Select(AssetRegistry.Register)
                .Where(i => i != null && i.Type != AssetType.Unknown)
                .ToList();

            Assert.Equal(2, visible.Count);
            Assert.Contains(visible, i => i!.FileName == "corridor.fbx");
            Assert.Contains(visible, i => i!.FileName == "corridor.mesh");
        }
        finally
        {
            AssetRegistry.Reset();
        }
    }
}
