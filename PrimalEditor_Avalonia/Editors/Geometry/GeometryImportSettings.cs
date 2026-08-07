using System.IO;
using System.Text.Json.Nodes;
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

    public float SmoothingAngle { get; set; } = 178f;
    public bool CalculateNormals { get; set; } = true;
    public bool CalculateTangents { get; set; } = true;
    public bool ReverseHandedness { get; set; } = false;
    public bool ImportEmbeddedTextures { get; set; } = false;
    public bool ImportAnimations { get; set; } = false;
    public bool CoalesceMeshes { get; set; } = true;

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
        w.Write(SmoothingAngle);
        w.Write(CalculateNormals);
        w.Write(CalculateTangents);
        w.Write(ReverseHandedness);
        w.Write(ImportEmbeddedTextures);
        w.Write(ImportAnimations);
        w.Write(CoalesceMeshes);
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
        ForceSingleLod = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length) SmoothingAngle = r.ReadSingle();
        if (r.BaseStream.Position < r.BaseStream.Length) CalculateNormals = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length) CalculateTangents = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length) ReverseHandedness = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length) ImportEmbeddedTextures = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length) ImportAnimations = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length) CoalesceMeshes = r.ReadBoolean();
    }

    public void FromJson(JsonObject settings)
    {
        if (settings.TryGetPropertyValue("LodMode", out var lodModeNode) && lodModeNode is JsonValue lodModeValue)
        {
            if (lodModeValue.TryGetValue(out int lodModeInt))
            {
                LodMode = (LodGenerationMode)lodModeInt;
            }
        }

        if (settings.TryGetPropertyValue("Lod0Threshold", out var lod0ThresholdNode) && lod0ThresholdNode is JsonValue lod0ThresholdValue)
        {
            if (lod0ThresholdValue.TryGetValue(out float lod0Threshold))
            {
                Lod0Threshold = lod0Threshold;
            }
        }

        if (settings.TryGetPropertyValue("Lod1Threshold", out var lod1ThresholdNode) && lod1ThresholdNode is JsonValue lod1ThresholdValue)
        {
            if (lod1ThresholdValue.TryGetValue(out float lod1Threshold))
            {
                Lod1Threshold = lod1Threshold;
            }
        }

        if (settings.TryGetPropertyValue("Lod2Threshold", out var lod2ThresholdNode) && lod2ThresholdNode is JsonValue lod2ThresholdValue)
        {
            if (lod2ThresholdValue.TryGetValue(out float lod2Threshold))
            {
                Lod2Threshold = lod2Threshold;
            }
        }

        if (settings.TryGetPropertyValue("GenerateTangentSpace", out var generateTangentSpaceNode) && generateTangentSpaceNode is JsonValue generateTangentSpaceValue)
        {
            if (generateTangentSpaceValue.TryGetValue(out bool generateTangentSpace))
            {
                GenerateTangentSpace = generateTangentSpace;
            }
        }

        if (settings.TryGetPropertyValue("RecalculateNormals", out var recalculateNormalsNode) && recalculateNormalsNode is JsonValue recalculateNormalsValue)
        {
            if (recalculateNormalsValue.TryGetValue(out bool recalculateNormals))
            {
                RecalculateNormals = recalculateNormals;
            }
        }

        if (settings.TryGetPropertyValue("Compression", out var compressionNode) && compressionNode is JsonValue compressionValue)
        {
            if (compressionValue.TryGetValue(out int compressionInt))
            {
                Compression = (MeshCompressionMode)compressionInt;
            }
        }

        if (settings.TryGetPropertyValue("ForceSingleLod", out var forceSingleLodNode) && forceSingleLodNode is JsonValue forceSingleLodValue)
        {
            if (forceSingleLodValue.TryGetValue(out bool forceSingleLod))
            {
                ForceSingleLod = forceSingleLod;
            }
        }

        if (settings.TryGetPropertyValue("SmoothingAngle", out var sN0) && sN0 is JsonValue vN0 && vN0.TryGetValue(out float fN0)) SmoothingAngle = fN0;
        if (settings.TryGetPropertyValue("CalculateNormals", out var sN1) && sN1 is JsonValue vN1 && vN1.TryGetValue(out bool bN1)) CalculateNormals = bN1;
        if (settings.TryGetPropertyValue("CalculateTangents", out var sN2) && sN2 is JsonValue vN2 && vN2.TryGetValue(out bool bN2)) CalculateTangents = bN2;
        if (settings.TryGetPropertyValue("ReverseHandedness", out var sN3) && sN3 is JsonValue vN3 && vN3.TryGetValue(out bool bN3)) ReverseHandedness = bN3;
        if (settings.TryGetPropertyValue("ImportEmbeddedTextures", out var sN4) && sN4 is JsonValue vN4 && vN4.TryGetValue(out bool bN4)) ImportEmbeddedTextures = bN4;
        if (settings.TryGetPropertyValue("ImportAnimations", out var sN5) && sN5 is JsonValue vN5 && vN5.TryGetValue(out bool bN5)) ImportAnimations = bN5;
        if (settings.TryGetPropertyValue("CoalesceMeshes", out var sN6) && sN6 is JsonValue vN6 && vN6.TryGetValue(out bool bN6)) CoalesceMeshes = bN6;
    }

    public JsonObject ToJson()
    {
        return new JsonObject
        {
            ["LodMode"] = (int)LodMode,
            ["Lod0Threshold"] = Lod0Threshold,
            ["Lod1Threshold"] = Lod1Threshold,
            ["Lod2Threshold"] = Lod2Threshold,
            ["GenerateTangentSpace"] = GenerateTangentSpace,
            ["RecalculateNormals"] = RecalculateNormals,
            ["Compression"] = (int)Compression,
            ["ForceSingleLod"] = ForceSingleLod,
            ["SmoothingAngle"] = SmoothingAngle,
            ["CalculateNormals"] = CalculateNormals,
            ["CalculateTangents"] = CalculateTangents,
            ["ReverseHandedness"] = ReverseHandedness,
            ["ImportEmbeddedTextures"] = ImportEmbeddedTextures,
            ["ImportAnimations"] = ImportAnimations,
            ["CoalesceMeshes"] = CoalesceMeshes
        };
    }
}
