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
            var magic = br.ReadUInt64();
            if (magic != 0x48534D4550474D2DUL)
            {
                LoadFallback(path);
                return LODs.Count > 0;
            }

            var lodCount = br.ReadUInt32();
            for (uint i = 0; i < lodCount; ++i)
            {
                var lod = new MeshLOD { Name = br.ReadString(), LodThreshold = br.ReadSingle() };
                var meshCount = br.ReadUInt32();
                for (uint m = 0; m < meshCount; ++m)
                {
                    var mesh = new Mesh { Name = br.ReadString() };
                    mesh.VertexCount  = (int)br.ReadUInt32();
                    mesh.IndexCount   = (int)br.ReadUInt32();
                    mesh.IndexSize    = (int)br.ReadUInt32();
                    mesh.ElementsType = (ElementsType)br.ReadUInt32();
                    var posBytes  = mesh.VertexCount * sizeof(float) * 3;
                    var elemBytes = (int)br.ReadUInt32();
                    mesh.Positions = br.ReadBytes(posBytes);
                    mesh.Elements  = br.ReadBytes(elemBytes);
                    mesh.Indices   = br.ReadBytes(mesh.IndexCount * mesh.IndexSize);
                    lod.Meshes.Add(mesh);
                }
                LODs.Add(lod);
            }
            return true;
        }
        catch (Exception ex)
        {
            System.Console.Error.WriteLine($"Geometry.Load failed: {ex.Message}");
            LoadFallback(path);
            return LODs.Count > 0;
        }
    }

    private void LoadFallback(string path)
    {
        try
        {
            var lod = new MeshLOD { Name = "default", LodThreshold = 0f };
            var mesh = new Mesh { Name = System.IO.Path.GetFileNameWithoutExtension(path) };
            mesh.VertexCount = 1;
            mesh.IndexCount = 0;
            mesh.Positions = new byte[sizeof(float) * 3];
            lod.Meshes.Add(mesh);
            LODs.Add(lod);
        }
        catch { }
    }
}