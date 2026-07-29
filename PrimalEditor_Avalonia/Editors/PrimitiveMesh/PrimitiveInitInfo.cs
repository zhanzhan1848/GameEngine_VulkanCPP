using System.IO;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public class PrimitiveInitInfo : IAssetImportSettings
{
    public PrimitiveMeshType Type { get; set; }
    public int SegmentsX { get; set; } = 1;
    public int SegmentsY { get; set; } = 1;
    public int SegmentsZ { get; set; } = 1;
    public float SizeX { get; set; } = 1f;
    public float SizeY { get; set; } = 1f;
    public float SizeZ { get; set; } = 1f;
    public int LOD { get; set; } = 0;
    public float SmoothingAngle { get; set; } = 178f;
    public string OutputName { get; set; } = "Primitive_Mesh";

    public void ToBinary(BinaryWriter w)
    {
        w.Write((int)Type);
        w.Write(SegmentsX);
        w.Write(SegmentsY);
        w.Write(SegmentsZ);
        w.Write(SizeX);
        w.Write(SizeY);
        w.Write(SizeZ);
        w.Write(LOD);
        w.Write(SmoothingAngle);
        w.Write(OutputName ?? "Primitive_Mesh");
    }

    public void FromBinary(BinaryReader r)
    {
        Type = (PrimitiveMeshType)r.ReadInt32();
        SegmentsX = r.ReadInt32();
        SegmentsY = r.ReadInt32();
        SegmentsZ = r.ReadInt32();
        SizeX = r.ReadSingle();
        SizeY = r.ReadSingle();
        SizeZ = r.ReadSingle();
        LOD = r.ReadInt32();
        SmoothingAngle = r.ReadSingle();
        if (r.BaseStream.Position < r.BaseStream.Length)
            OutputName = r.ReadString() ?? "Primitive_Mesh";
    }
}