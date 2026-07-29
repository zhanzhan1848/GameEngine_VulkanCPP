using System.IO;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Editors.Geometry;

public enum LodGenerationMode { Manual, Auto }
public enum MeshCompressionMode { None, Auto }

public class GeometryImportSettings : IAssetImportSettings
{
    public LodGenerationMode LodMode { get; set; } = LodGenerationMode.Manual;
    public float Lod0Threshold { get; set; } = 0f;
    public float Lod1Threshold { get; set; } = 50f;
    public float Lod2Threshold { get; set; } = 100f;
    public bool GenerateTangentSpace { get; set; } = true;
    public bool RecalculateNormals { get; set; } = false;
    public MeshCompressionMode Compression { get; set; } = MeshCompressionMode.None;
    public bool ForceSingleLod { get; set; } = false;

    public void ToBinary(BinaryWriter w)
    {
        w.Write((int)LodMode);
        w.Write(Lod0Threshold);
        w.Write(Lod1Threshold);
        w.Write(Lod2Threshold);
        w.Write(GenerateTangentSpace);
        w.Write(RecalculateNormals);
        w.Write((int)Compression);
        w.Write(ForceSingleLod);
    }

    public void FromBinary(BinaryReader r)
    {
        LodMode = (LodGenerationMode)r.ReadInt32();
        Lod0Threshold = r.ReadSingle();
        Lod1Threshold = r.ReadSingle();
        Lod2Threshold = r.ReadSingle();
        GenerateTangentSpace = r.ReadBoolean();
        RecalculateNormals = r.ReadBoolean();
        Compression = (MeshCompressionMode)r.ReadInt32();
        if (r.BaseStream.Position < r.BaseStream.Length)
            ForceSingleLod = r.ReadBoolean();
    }
}
