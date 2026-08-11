using System;

namespace PrimalEditor_Avalonia.Editors.Geometry;

public enum PrimitiveTopology { PointList = 1, LineList, LineStrip, TriangleList, TriangleStrip }

[System.Flags]
public enum ElementsType { Position = 0x00, Normals = 0x01, TSpace = 0x03, Joints = 0x04, Colors = 0x08 }

public class Mesh
{
    public string Name { get; set; } = "";
    public int VertexCount { get; set; }
    public int IndexCount { get; set; }
    public int IndexSize { get; set; }
    public ElementsType ElementsType { get; set; }
    public byte[] Positions { get; set; } = System.Array.Empty<byte>();
    public byte[] Elements  { get; set; } = System.Array.Empty<byte>();
    public byte[] Indices   { get; set; } = System.Array.Empty<byte>();

    public float[] GetPositionsAsFloat3()
    {
        var arr = new float[VertexCount * 3];
        System.Buffer.BlockCopy(Positions, 0, arr, 0, Positions.Length);
        return arr;
    }

    public int[] GetIndicesAsInt()
    {
        var arr = new int[IndexCount];
        if (IndexSize == 2)
            for (int i = 0; i < IndexCount; i++) arr[i] = System.BitConverter.ToUInt16(Indices, i * 2);
        else
            for (int i = 0; i < IndexCount; i++) arr[i] = System.BitConverter.ToInt32(Indices, i * 4);
        return arr;
    }
}

public class MeshLOD
{
    public string Name { get; set; } = "";
    public float LodThreshold { get; set; }
    public System.Collections.Generic.List<Mesh> Meshes { get; } = new();
}

public class Geometry
{
    public string FullPath { get; set; } = "";
    public System.Collections.Generic.List<MeshLOD> LODs { get; } = new();

    public bool Load(string path)
    {
        if (!System.IO.File.Exists(path)) return false;
        LODs.Clear();
        FullPath = path;

        try
        {
            using var fs = System.IO.File.OpenRead(path);
            using var br = new System.IO.BinaryReader(fs);

            // Wire format mirrors ContentTools/Geometry.cpp::pack_data:
            //   scene_name (u32 len + bytes)
            //   material_count (u32) + per-material {name, diffuse_tex, normal_tex}
            //   lod_count (u32) + per-lod {name, mesh_count, meshes}
            //   per-mesh {name, lod_id, material_idx, element_size, elements_type,
            //             num_vertices, index_size, num_indices, lod_threshold,
            //             position_buffer, element_buffer, index_buffer,
            //             MSHL meshlets block, SDF block}
            ReadU32LengthString(br); // scene name
            var materialCount = br.ReadUInt32();
            for (uint i = 0; i < materialCount; ++i)
            {
                ReadU32LengthString(br);
                ReadU32LengthString(br);
                ReadU32LengthString(br);
            }

            var lodCount = br.ReadUInt32();
            for (uint i = 0; i < lodCount; ++i)
            {
                var lodName = ReadU32LengthString(br);
                var meshCount = br.ReadUInt32();
                var lod = new MeshLOD { Name = lodName, LodThreshold = 0f };
                for (uint m = 0; m < meshCount; ++m)
                {
                    var mesh = new Mesh
                    {
                        Name = ReadU32LengthString(br),
                    };
                    br.ReadUInt32();  // lod_id (unused in CPU preview)
                    br.ReadUInt32();  // material_idx
                    var elementSize = (int)br.ReadUInt32();
                    mesh.ElementsType = (ElementsType)br.ReadUInt32();
                    mesh.VertexCount  = (int)br.ReadUInt32();
                    mesh.IndexSize    = (int)br.ReadUInt32();
                    mesh.IndexCount   = (int)br.ReadUInt32();
                    lod.LodThreshold  = br.ReadSingle();

                    var posBytes = mesh.VertexCount * sizeof(float) * 3;
                    mesh.Positions = br.ReadBytes(posBytes);
                    mesh.Elements  = br.ReadBytes(mesh.VertexCount * elementSize);
                    mesh.Indices   = br.ReadBytes(mesh.IndexCount * mesh.IndexSize);

                    SkipMeshletBlock(br);
                    SkipSdfBlock(br);

                    lod.Meshes.Add(mesh);
                }
                LODs.Add(lod);
            }
            return LODs.Count > 0;
        }
        catch (Exception ex)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning($"Geometry.Load failed: {ex.Message}");
            return false;
        }
    }

    private static string ReadU32LengthString(System.IO.BinaryReader br)
    {
        var len = (int)br.ReadUInt32();
        if (len <= 0) return string.Empty;
        var bytes = br.ReadBytes(len);
        return System.Text.Encoding.UTF8.GetString(bytes);
    }

    private static void SkipMeshletBlock(System.IO.BinaryReader br)
    {
        var magic = br.ReadUInt32();
        if (magic != 0x4C48534D) // 'MSHL' (LE)
        {
            br.BaseStream.Position -= 4;
            return;
        }
        var meshletCount = (int)br.ReadUInt32();
        // sizeof(mesh::meshlet) = 4 vertex_offset + 4 triangle_offset +
        //   4 vertex_count + 4 triangle_count + 12 cone_apex + 12 cone_axis
        //   + 4 cone_cutoff + 12 center + 4 radius = 60 bytes
        br.BaseStream.Seek(meshletCount * 60, System.IO.SeekOrigin.Current);
        var meshletVertCount = (int)br.ReadUInt32();
        br.BaseStream.Seek(meshletVertCount * 4, System.IO.SeekOrigin.Current);
        var meshletTriCount = (int)br.ReadUInt32();
        br.BaseStream.Seek(meshletTriCount, System.IO.SeekOrigin.Current);
    }

    private static void SkipSdfBlock(System.IO.BinaryReader br)
    {
        var magic = br.ReadUInt32();
        if (magic != 0x20464453) // 'SDF ' (LE)
        {
            br.BaseStream.Position -= 4;
            return;
        }
        br.BaseStream.Seek(sizeof(uint) * 3, System.IO.SeekOrigin.Current);
        br.BaseStream.Seek(sizeof(float) * 6, System.IO.SeekOrigin.Current);
        var sdfDataSize = (int)br.ReadUInt32();
        br.BaseStream.Seek(sdfDataSize * sizeof(ushort), System.IO.SeekOrigin.Current);
        var voxelSize = (int)br.ReadUInt32();
        br.BaseStream.Seek(voxelSize, System.IO.SeekOrigin.Current);
        var vectorFieldSize = (int)br.ReadUInt32();
        br.BaseStream.Seek(vectorFieldSize * sizeof(ushort), System.IO.SeekOrigin.Current);
    }
}