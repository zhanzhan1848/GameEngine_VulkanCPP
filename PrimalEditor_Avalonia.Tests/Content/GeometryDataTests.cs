using System.IO;
using System.Text;
using PrimalEditor_Avalonia.Editors.Geometry;
using MeshGeometry = PrimalEditor_Avalonia.Editors.Geometry.Geometry;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

public sealed class GeometryDataTests
{
    [Fact]
    public void Load_Reads_Real_Mesh_Binary_Format()
    {
        using var temp = new TestInfrastructure.TemporaryDirectory();
        var path = Path.Combine(temp.Path, "test.mesh");

        // Build a minimal scene_data blob that matches the C++ writer
        // (ContentTools/Geometry.cpp::pack_data) field-by-field.
        // 1 scene name, 0 materials, 1 LOD with 2 meshes.
        using var fs = File.Create(path);
        using var bw = new BinaryWriter(fs);

        WriteU32String(bw, "Hero");
        bw.Write(0u);                 // material_count
        bw.Write(1u);                 // lod_count
        WriteU32String(bw, "LOD0");
        bw.Write(2u);                 // mesh_count in LOD

        // mesh 1: "Body" with 4 verts, 6 indices (2 triangles)
        WriteMesh(bw, name: "Body", lodId: 0, matIdx: 0,
                  elementSize: 20, elementsType: 3,
                  numVertices: 4, indexSize: 4, numIndices: 6,
                  lodThreshold: 0f);

        // mesh 2: "Head" with 8 verts, 12 indices
        // Give mesh 2 some bytes in its MSHL/SDF blocks to make sure
        // the skip math is exercised with non-zero payload sizes.
        WriteMesh(bw, name: "Head", lodId: 0, matIdx: 1,
                  elementSize: 20, elementsType: 3,
                  numVertices: 8, indexSize: 4, numIndices: 12,
                  lodThreshold: 0f,
                  meshletBytes: 60, meshletVertBytes: 12, meshletTriBytes: 5,
                  sdfDataBytes: 6, voxelBytes: 4, vectorFieldBytes: 8);
        bw.Flush();
        // The using-declaration will close fs/bw at the end of this scope;
        // we need to read the file outside that scope, so close them now.
        bw.Dispose();
        fs.Dispose();

        var geom = new MeshGeometry();
        var loaded = geom.Load(path);
        if (!loaded)
        {
            // Diagnostic: print first 200 bytes hex
            var bytes = File.ReadAllBytes(path);
            var hex = new System.Text.StringBuilder();
            for (int i = 0; i < System.Math.Min(bytes.Length, 200); ++i)
                hex.AppendFormat("{0:x2} ", bytes[i]);
            Assert.Fail($"Geometry.Load returned false. First bytes: {hex}");
        }
        // Diagnostic: dump what we got
        var dump = new System.Text.StringBuilder();
        for (int i = 0; i < geom.LODs.Count; i++)
        {
            dump.AppendFormat("LOD[{0}] '{1}': {2} mesh(es)\n", i, geom.LODs[i].Name, geom.LODs[i].Meshes.Count);
            for (int m = 0; m < geom.LODs[i].Meshes.Count; m++)
            {
                var me = geom.LODs[i].Meshes[m];
                dump.AppendFormat("  mesh[{0}] '{1}' v={2} i={3} pos={4}B elem={5}B idx={6}B\n",
                    m, me.Name, me.VertexCount, me.IndexCount, me.Positions.Length, me.Elements.Length, me.Indices.Length);
            }
        }
        // Continue with assertions (will fail with diagnostic on mismatch).
        Assert.True(geom.LODs.Count >= 1, dump.ToString());
        Assert.Equal(2, geom.LODs[0].Meshes.Count);
        Assert.Equal("Body", geom.LODs[0].Meshes[0].Name);
        Assert.Equal(4, geom.LODs[0].Meshes[0].VertexCount);
        Assert.Equal(6, geom.LODs[0].Meshes[0].IndexCount);
        Assert.Equal(12 * 4, geom.LODs[0].Meshes[0].Positions.Length); // xyz f32 * 4
        Assert.Equal(20 * 4, geom.LODs[0].Meshes[0].Elements.Length);
        Assert.Equal(6 * 4, geom.LODs[0].Meshes[0].Indices.Length);
        Assert.Equal("Head", geom.LODs[0].Meshes[1].Name);
        Assert.Equal(8, geom.LODs[0].Meshes[1].VertexCount);
    }

    [Fact]
    public void Load_Returns_False_For_Missing_File()
    {
        var geom = new MeshGeometry();
        Assert.False(geom.Load("/tmp/missing-" + System.Guid.NewGuid() + ".mesh"));
    }

    [Fact]
    public void Load_Returns_False_For_Truncated_File()
    {
        using var temp = new TestInfrastructure.TemporaryDirectory();
        var path = Path.Combine(temp.Path, "truncated.mesh");
        File.WriteAllBytes(path, new byte[] { 1, 2, 3, 4 }); // garbage
        var geom = new MeshGeometry();
        Assert.False(geom.Load(path));
    }

    private static void WriteU32String(BinaryWriter bw, string s)
    {
        var bytes = Encoding.UTF8.GetBytes(s);
        bw.Write((uint)bytes.Length);
        bw.Write(bytes);
    }

    private static void WriteMesh(BinaryWriter bw, string name, uint lodId, uint matIdx,
        uint elementSize, uint elementsType, uint numVertices, uint indexSize, uint numIndices,
        float lodThreshold,
        int meshletBytes = 0, int meshletVertBytes = 0, int meshletTriBytes = 0,
        int sdfDataBytes = 0, int voxelBytes = 0, int vectorFieldBytes = 0)
    {
        WriteU32String(bw, name);
        bw.Write(lodId);
        bw.Write(matIdx);
        bw.Write(elementSize);
        bw.Write(elementsType);
        bw.Write(numVertices);
        bw.Write(indexSize);
        bw.Write(numIndices);
        bw.Write(lodThreshold);

        // Position buffer: 12 * numVertices bytes
        bw.Write(new byte[12 * (int)numVertices]);
        // Element buffer: elementSize * numVertices bytes
        bw.Write(new byte[(int)elementSize * (int)numVertices]);
        // Index buffer: indexSize * numIndices bytes
        bw.Write(new byte[(int)indexSize * (int)numIndices]);

        // MSHL meshlet block. Each meshlet is 60 bytes (sizeof mesh::meshlet
        // on the C++ side: see Geometry.h mesh::meshlet for the layout).
        bw.Write(0x4C48534Du); // 'MSHL' LE
        var meshletCount = (uint)(meshletBytes / 60);
        bw.Write(meshletCount);
        bw.Write(new byte[meshletBytes]);
        var meshletVertCount = (uint)(meshletVertBytes / 4);
        bw.Write(meshletVertCount);
        bw.Write(new byte[meshletVertBytes]);
        var meshletTriCount = (uint)meshletTriBytes;
        bw.Write(meshletTriCount);
        bw.Write(new byte[meshletTriBytes]);

        // SDF block
        bw.Write(0x20464453u); // 'SDF ' LE
        for (int i = 0; i < 3; ++i) bw.Write(0u);  // resolution
        for (int i = 0; i < 6; ++i) bw.Write(0f);  // bounds
        var sdfDataCount = (uint)(sdfDataBytes / 2);
        bw.Write(sdfDataCount);
        bw.Write(new byte[sdfDataBytes]);
        bw.Write((uint)voxelBytes);
        bw.Write(new byte[voxelBytes]);
        var vectorFieldCount = (uint)(vectorFieldBytes / 2);
        bw.Write(vectorFieldCount);
        bw.Write(new byte[vectorFieldBytes]);
    }
}
