using System;
using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

public sealed class WorldEditorAssetDropTests
{
    [Fact]
    public void ResolveDropTarget_ModelJson_UsesSiblingCookedMesh()
    {
        var directory = Path.Combine(Path.GetTempPath(), "PrimalDrop_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var sidecar = Path.Combine(directory, "corridor-corner.model.json");
            var cookedMesh = Path.Combine(directory, "corridor-corner.mesh");
            File.WriteAllText(sidecar, "{}");
            File.WriteAllBytes(cookedMesh, Array.Empty<byte>());

            var result = WorldEditorViewModel.ResolveDropTarget(new AssetInfo { FullPath = sidecar });

            Assert.Equal(cookedMesh, result.nativePath);
            Assert.Equal("corridor-corner", result.entityName);
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public void ResolveDropTarget_ModelJson_ReturnsNullWhenCookedMeshIsMissing()
    {
        var sidecar = Path.Combine(Path.GetTempPath(), "missing-" + Guid.NewGuid().ToString("N") + ".model.json");

        var result = WorldEditorViewModel.ResolveDropTarget(new AssetInfo { FullPath = sidecar });

        Assert.Null(result.nativePath);
        Assert.StartsWith("missing-", result.entityName);
        Assert.DoesNotContain(".model", result.entityName);
    }

    [Fact]
    public void ResolveDropTarget_Mesh_UsesAssetPathDirectly()
    {
        var mesh = Path.Combine(Path.GetTempPath(), "crate.mesh");

        var result = WorldEditorViewModel.ResolveDropTarget(new AssetInfo { FullPath = mesh });

        Assert.Equal(mesh, result.nativePath);
        Assert.Equal("crate", result.entityName);
    }
}
