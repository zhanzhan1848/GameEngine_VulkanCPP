using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class AssetRegistryModelChildrenTests
{
    [Fact]
    public void Register_Model_Asset_Populates_Children_From_Sidecar()
    {
        using var temp = new TestInfrastructure.TemporaryDirectory();
        var contentRoot = Path.Combine(temp.Path, "Content");
        Directory.CreateDirectory(contentRoot);
        var assetsDir = Path.Combine(contentRoot, "Assets");
        Directory.CreateDirectory(assetsDir);

        var modelPath = Path.Combine(assetsDir, "hero.model");
        var sidecar = new ModelSidecar
        {
            Source = "/src/hero.fbx",
            Mesh = Path.Combine(assetsDir, "hero.mesh"),
            Submeshes =
            {
                new ModelSubmeshEntry { Name = "Body", LodId = 0, LodIndex = 0, MeshIndex = 0, MaterialIndex = 0, VertexCount = 4, IndexCount = 6 },
                new ModelSubmeshEntry { Name = "Head", LodId = 0, LodIndex = 0, MeshIndex = 1, MaterialIndex = 1, VertexCount = 8, IndexCount = 12 },
                new ModelSubmeshEntry { Name = "Arm.L", LodId = 0, LodIndex = 0, MeshIndex = 2, MaterialIndex = 2, VertexCount = 3, IndexCount = 3 },
            }
        };
        sidecar.Save(modelPath);

        try
        {
            AssetRegistry.Initialize(contentRoot);
            var info = AssetRegistry.Register(modelPath);

            Assert.NotNull(info);
            Assert.Equal(AssetType.Model, info!.Type);
            Assert.True(info.IsContainer);
            Assert.Equal(3, info.Children.Count);
            // FileName includes the synthetic .submesh directory; the
            // last path segment is the submesh name verbatim.
            Assert.Equal("Body", info.Children[0].FileName);
            Assert.Equal("Head", info.Children[1].FileName);
            Assert.Equal("Arm.L", info.Children[2].FileName);
            Assert.All(info.Children, c => Assert.Equal(AssetType.Mesh, c.Type));
        }
        finally
        {
            AssetRegistry.Reset();
        }
    }

    [Fact]
    public void Register_Model_Without_Sidecar_Has_Empty_Children()
    {
        using var temp = new TestInfrastructure.TemporaryDirectory();
        var contentRoot = Path.Combine(temp.Path, "Content");
        Directory.CreateDirectory(contentRoot);

        var modelPath = Path.Combine(contentRoot, "lone.model");
        File.WriteAllText(modelPath, "{}"); // not a real sidecar, Load returns null

        try
        {
            AssetRegistry.Initialize(contentRoot);
            var info = AssetRegistry.Register(modelPath);
            Assert.NotNull(info);
            Assert.Equal(AssetType.Model, info!.Type);
            Assert.Empty(info.Children);
        }
        finally
        {
            AssetRegistry.Reset();
        }
    }
}
