using System;
using System.IO;

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
        if (!File.Exists(path)) return false;
        LODs.Clear();
        FullPath = path;

        try
        {
            using var fs = File.OpenRead(path);
            using var br = new BinaryReader(fs);

            // Wire format mirrors ContentTools/Geometry.cpp::pack_data:
            //   scene_name (u32 len + bytes)
            //   material_count (u32) + per-material {name, diffuse_tex, normal_tex}
            //   lod_count (u32) + per-lod {name, mesh_count, meshes}
            //   per-mesh {name, lod_id, material_idx, element_size, elements_type,
            //             num_vertices, index_size, num_indices, lod_threshold,
            //             position_buffer, element_buffer, index_buffer,
            //             MSHL meshlets block, SDF block}
            ReadU32LengthString(br); // scene name
            var materialCount = ReadBoundedCount(br, minimumBytesPerItem: 12, "material");
            for (uint i = 0; i < materialCount; ++i)
            {
                ReadU32LengthString(br);
                ReadU32LengthString(br);
                ReadU32LengthString(br);
            }

            var lodCount = ReadBoundedCount(br, minimumBytesPerItem: 8, "LOD");
            for (uint i = 0; i < lodCount; ++i)
            {
                var lodName = ReadU32LengthString(br);
                var meshCount = ReadBoundedCount(br, minimumBytesPerItem: 40, "mesh");
                var lod = new MeshLOD { Name = lodName, LodThreshold = 0f };
                for (uint m = 0; m < meshCount; ++m)
                {
                    var mesh = new Mesh
                    {
                        Name = ReadU32LengthString(br),
                    };
                    br.ReadUInt32();  // lod_id (unused in CPU preview)
                    br.ReadUInt32();  // material_idx
                    var elementSize = br.ReadUInt32();
                    mesh.ElementsType = (ElementsType)br.ReadUInt32();
                    var vertexCount = br.ReadUInt32();
                    var indexSize = br.ReadUInt32();
                    var indexCount = br.ReadUInt32();
                    lod.LodThreshold  = br.ReadSingle();

                    if (indexSize is not (sizeof(ushort) or sizeof(uint)))
                        throw new InvalidDataException($"Unsupported mesh index size: {indexSize}");

                    mesh.VertexCount = CheckedInt(vertexCount, "vertex count");
                    mesh.IndexSize = (int)indexSize;
                    mesh.IndexCount = CheckedInt(indexCount, "index count");

                    var posBytes = CheckedByteCount(vertexCount, sizeof(float) * 3, "position buffer");
                    var elementBytes = CheckedByteCount(vertexCount, elementSize, "element buffer");
                    var indexBytes = CheckedByteCount(indexCount, indexSize, "index buffer");
                    mesh.Positions = ReadExactBytes(br, posBytes, "position buffer");
                    mesh.Elements  = ReadExactBytes(br, elementBytes, "element buffer");
                    mesh.Indices   = ReadExactBytes(br, indexBytes, "index buffer");

                    ValidateIndices(mesh);

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
            LODs.Clear();
            PrimalEditor_Avalonia.Common.Logger.Warning($"Geometry.Load failed: {ex.Message}");
            return false;
        }
    }

    private static string ReadU32LengthString(BinaryReader br)
    {
        var len = CheckedByteCount(br.ReadUInt32(), 1, "string");
        if (len == 0) return string.Empty;
        var bytes = ReadExactBytes(br, len, "string");
        return System.Text.Encoding.UTF8.GetString(bytes);
    }

    private static void SkipMeshletBlock(BinaryReader br)
    {
        var magic = br.ReadUInt32();
        if (magic != 0x4C48534D) // 'MSHL' (LE)
        {
            br.BaseStream.Position -= 4;
            return;
        }
        var meshletCount = br.ReadUInt32();
        // sizeof(mesh::meshlet) = 4 vertex_offset + 4 triangle_offset +
        //   4 vertex_count + 4 triangle_count + 12 cone_apex + 12 cone_axis
        //   + 4 cone_cutoff + 12 center + 4 radius = 60 bytes
        SkipExact(br, CheckedByteCount(meshletCount, 60, "meshlet buffer"), "meshlet buffer");
        var meshletVertCount = br.ReadUInt32();
        SkipExact(br, CheckedByteCount(meshletVertCount, sizeof(uint), "meshlet vertex buffer"), "meshlet vertex buffer");
        var meshletTriCount = br.ReadUInt32();
        SkipExact(br, CheckedByteCount(meshletTriCount, 1, "meshlet triangle buffer"), "meshlet triangle buffer");
    }

    private static void SkipSdfBlock(BinaryReader br)
    {
        var magic = br.ReadUInt32();
        if (magic != 0x20464453) // 'SDF ' (LE)
        {
            br.BaseStream.Position -= 4;
            return;
        }
        SkipExact(br, sizeof(uint) * 3 + sizeof(float) * 6, "SDF header");
        var sdfDataSize = br.ReadUInt32();
        SkipExact(br, CheckedByteCount(sdfDataSize, sizeof(ushort), "SDF data"), "SDF data");
        var voxelSize = br.ReadUInt32();
        SkipExact(br, CheckedByteCount(voxelSize, 1, "SDF voxel data"), "SDF voxel data");
        var vectorFieldSize = br.ReadUInt32();
        SkipExact(br, CheckedByteCount(vectorFieldSize, sizeof(ushort), "SDF vector field"), "SDF vector field");
    }

    private static uint ReadBoundedCount(BinaryReader br, int minimumBytesPerItem, string label)
    {
        var count = br.ReadUInt32();
        var remaining = br.BaseStream.Length - br.BaseStream.Position;
        if ((ulong)count * (uint)minimumBytesPerItem > (ulong)remaining)
            throw new InvalidDataException($"Invalid {label} count: {count}");
        return count;
    }

    private static int CheckedInt(uint value, string label)
    {
        if (value > int.MaxValue)
            throw new InvalidDataException($"Invalid {label}: {value}");
        return (int)value;
    }

    private static int CheckedByteCount(uint count, uint stride, string label)
    {
        var size = (ulong)count * stride;
        if (size > int.MaxValue)
            throw new InvalidDataException($"{label} is too large: {size} bytes");
        return (int)size;
    }

    private static byte[] ReadExactBytes(BinaryReader br, int count, string label)
    {
        if (count < 0 || count > br.BaseStream.Length - br.BaseStream.Position)
            throw new InvalidDataException($"Truncated {label}: expected {count} bytes");
        var bytes = br.ReadBytes(count);
        if (bytes.Length != count)
            throw new EndOfStreamException($"Truncated {label}: expected {count} bytes, read {bytes.Length}");
        return bytes;
    }

    private static void SkipExact(BinaryReader br, int count, string label)
    {
        if (count < 0 || count > br.BaseStream.Length - br.BaseStream.Position)
            throw new InvalidDataException($"Truncated {label}: expected {count} bytes");
        br.BaseStream.Seek(count, SeekOrigin.Current);
    }

    private static void ValidateIndices(Mesh mesh)
    {
        for (var i = 0; i < mesh.IndexCount; ++i)
        {
            var index = mesh.IndexSize == sizeof(ushort)
                ? BitConverter.ToUInt16(mesh.Indices, i * sizeof(ushort))
                : BitConverter.ToUInt32(mesh.Indices, i * sizeof(uint));
            if (index >= mesh.VertexCount)
                throw new InvalidDataException(
                    $"Mesh index {index} at position {i} exceeds vertex count {mesh.VertexCount}");
        }
    }
}
