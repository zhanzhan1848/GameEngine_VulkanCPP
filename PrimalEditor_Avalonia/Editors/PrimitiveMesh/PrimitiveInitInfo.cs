namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public class PrimitiveInitInfo
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
}