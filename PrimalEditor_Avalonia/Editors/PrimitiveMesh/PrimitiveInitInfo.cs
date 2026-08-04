using System.IO;
using System.Text.Json.Nodes;
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

    public void FromJson(JsonObject settings)
    {
        if (settings.TryGetPropertyValue("Type", out var typeNode) && typeNode is JsonValue typeValue)
        {
            if (typeValue.TryGetValue(out int typeInt))
            {
                Type = (PrimitiveMeshType)typeInt;
            }
        }

        if (settings.TryGetPropertyValue("SegmentsX", out var segmentsXNode) && segmentsXNode is JsonValue segmentsXValue)
        {
            if (segmentsXValue.TryGetValue(out int segmentsX))
            {
                SegmentsX = segmentsX;
            }
        }

        if (settings.TryGetPropertyValue("SegmentsY", out var segmentsYNode) && segmentsYNode is JsonValue segmentsYValue)
        {
            if (segmentsYValue.TryGetValue(out int segmentsY))
            {
                SegmentsY = segmentsY;
            }
        }

        if (settings.TryGetPropertyValue("SegmentsZ", out var segmentsZNode) && segmentsZNode is JsonValue segmentsZValue)
        {
            if (segmentsZValue.TryGetValue(out int segmentsZ))
            {
                SegmentsZ = segmentsZ;
            }
        }

        if (settings.TryGetPropertyValue("SizeX", out var sizeXNode) && sizeXNode is JsonValue sizeXValue)
        {
            if (sizeXValue.TryGetValue(out float sizeX))
            {
                SizeX = sizeX;
            }
        }

        if (settings.TryGetPropertyValue("SizeY", out var sizeYNode) && sizeYNode is JsonValue sizeYValue)
        {
            if (sizeYValue.TryGetValue(out float sizeY))
            {
                SizeY = sizeY;
            }
        }

        if (settings.TryGetPropertyValue("SizeZ", out var sizeZNode) && sizeZNode is JsonValue sizeZValue)
        {
            if (sizeZValue.TryGetValue(out float sizeZ))
            {
                SizeZ = sizeZ;
            }
        }

        if (settings.TryGetPropertyValue("LOD", out var lodNode) && lodNode is JsonValue lodValue)
        {
            if (lodValue.TryGetValue(out int lod))
            {
                LOD = lod;
            }
        }

        if (settings.TryGetPropertyValue("SmoothingAngle", out var smoothingAngleNode) && smoothingAngleNode is JsonValue smoothingAngleValue)
        {
            if (smoothingAngleValue.TryGetValue(out float smoothingAngle))
            {
                SmoothingAngle = smoothingAngle;
            }
        }

        if (settings.TryGetPropertyValue("OutputName", out var outputNameNode) && outputNameNode is JsonValue outputNameValue)
        {
            if (outputNameValue.TryGetValue(out string? outputName))
            {
                OutputName = outputName ?? "Primitive_Mesh";
            }
        }
    }

    public JsonObject ToJson()
    {
        return new JsonObject
        {
            ["Type"] = (int)Type,
            ["SegmentsX"] = SegmentsX,
            ["SegmentsY"] = SegmentsY,
            ["SegmentsZ"] = SegmentsZ,
            ["SizeX"] = SizeX,
            ["SizeY"] = SizeY,
            ["SizeZ"] = SizeZ,
            ["LOD"] = LOD,
            ["SmoothingAngle"] = SmoothingAngle,
            ["OutputName"] = OutputName ?? "Primitive_Mesh"
        };
    }
}