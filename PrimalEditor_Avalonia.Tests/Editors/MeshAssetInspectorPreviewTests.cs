using System;
using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.MeshAssetInspector;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Editors;

/// <summary>
/// Regression coverage for the double-click model container (.model.json)
/// editor: the cooked-mesh fallback mapping and the inspector VM that
/// drives the wireframe preview.
/// </summary>
public sealed class MeshAssetInspectorPreviewTests
{
    // -- SiblingMeshPath -------------------------------------------------

    [Fact]
    public void Sidecar_Maps_To_Stripped_Sibling_Mesh()
    {
        Assert.Equal("/x/y/foo.mesh", ModelSidecar.SiblingMeshPath("/x/y/foo.model.json"));
        Assert.Equal("C:\\assets\\bar.mesh", ModelSidecar.SiblingMeshPath("C:\\assets\\bar.model.json"));
    }

    [Fact]
    public void NonSidecar_Extension_Falls_Back_To_ChangeExtension()
    {
        Assert.Equal("/x/y/foo.mesh", ModelSidecar.SiblingMeshPath("/x/y/foo.json"));
    }

    [Fact]
    public void ChangeExtension_Alone_Would_Produce_The_Old_Bug()
    {
        // The inspector previously resolved "foo.model.json" via
        // Path.ChangeExtension → "foo.model.mesh", which never exists.
        Assert.Equal("foo.model.mesh", Path.ChangeExtension("foo.model.json", ".mesh"));
    }

    // -- Inspector VM over a synthetic cooked mesh -----------------------

    [Fact]
    public void Inspector_Loads_Geometry_And_Tracks_Submesh_Selection()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalMeshInspector_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var meshPath = Path.Combine(dir, "tiny.mesh");
            WriteMinimalMesh(meshPath, vertexCount: 3);

            var sidecarPath = Path.Combine(dir, "tiny.model.json");
            var sidecar = new ModelSidecar
            {
                Version = 1,
                Source = "tiny.fbx",
                Mesh = meshPath,
                Submeshes = { new ModelSubmeshEntry { Name = "tiny", MeshIndex = 0, VertexCount = 3, IndexCount = 3 } },
            };
            sidecar.Save(sidecarPath);

            var vm = new MeshAssetInspectorViewModel(new AssetInfo
            {
                FullPath = sidecarPath,
                Type = AssetType.Model,
            });

            Assert.True(vm.HasGeometry, $"status: {vm.StatusMessage}");
            Assert.Equal(1, vm.LodCount);
            Assert.Equal(1, vm.SubmeshCount);

            // Root → Meshes folder → submesh 0.
            var submeshNode = vm.TreeRoots[0].Children[0].Children[0];
            vm.SelectedNode = submeshNode;
            Assert.Equal(0, vm.SelectedSubmeshIndex);

            vm.SelectedNode = vm.TreeRoots[0];
            Assert.Null(vm.SelectedSubmeshIndex);
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    [Fact]
    public void Inspector_Reports_Missing_Cooked_Mesh()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalMeshInspector_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            // Sidecar whose mesh field points nowhere AND no sibling
            // mesh on disk — the double-click preview must degrade to a
            // message, not an empty pane with no explanation.
            var sidecarPath = Path.Combine(dir, "ghost.model.json");
            new ModelSidecar { Mesh = Path.Combine(dir, "ghost.mesh") }.Save(sidecarPath);

            var vm = new MeshAssetInspectorViewModel(new AssetInfo
            {
                FullPath = sidecarPath,
                Type = AssetType.Model,
            });

            Assert.False(vm.HasGeometry);
            Assert.Contains(".mesh", vm.StatusMessage);
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    /// <summary>
    /// Author the minimal cooked-mesh binary the Geometry parser reads
    /// (ContentTools/Geometry.cpp::pack_data wire format): one LOD with
    /// one triangle-list submesh, no meshlet/SDF blocks.
    /// </summary>
    private static void WriteMinimalMesh(string path, int vertexCount)
    {
        using var fs = File.Create(path);
        using var bw = new BinaryWriter(fs);

        WriteString(bw, "scene");                       // scene name

        bw.Write(0u);                                   // material_count

        bw.Write(1u);                                   // lod_count
        WriteString(bw, "LOD0");                        // lod name
        bw.Write(1u);                                   // mesh_count
        WriteString(bw, "submesh0");                    // mesh name
        bw.Write(0xFFFFFFFFu);                          // lod_id
        bw.Write(0u);                                   // material_idx
        bw.Write(12u);                                  // element_size
        bw.Write(0u);                                   // elements_type
        bw.Write((uint)vertexCount);                    // num_vertices
        bw.Write(4u);                                   // index_size
        bw.Write(3u);                                   // num_indices
        bw.Write(0.5f);                                 // lod_threshold

        for (var i = 0; i < vertexCount; ++i)           // position buffer
        {
            bw.Write(i * 0.5f);
            bw.Write(i % 2 == 0 ? 0f : 1f);
            bw.Write(-i * 0.25f);
        }
        for (var i = 0; i < vertexCount; ++i)           // element buffer
        {
            bw.Write(1f); bw.Write(0f); bw.Write(0f);
        }
        bw.Write(0u);                                   // index buffer (triangle)
        bw.Write(1u);
        bw.Write(2u);

        bw.Write(0u);                                   // no MSHL / no SDF block
    }

    private static void WriteString(BinaryWriter bw, string s)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(s);
        bw.Write((uint)bytes.Length);
        bw.Write(bytes);
    }
}
