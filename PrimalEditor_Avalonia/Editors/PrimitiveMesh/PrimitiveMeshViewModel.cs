using System.Collections.Generic;
using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public class PrimitiveMeshViewModel : AssetEditorViewModel
{
    private PrimitiveMeshType _type = PrimitiveMeshType.Plane;
    public PrimitiveMeshType Type
    {
        get => _type;
        set
        {
            if (_type == value) return;
            _type = value;
            ApplyDefaultsForType();
            OnPropertyChanged();
            OnPropertyChanged(nameof(SegmentsX));
            OnPropertyChanged(nameof(SegmentsY));
            OnPropertyChanged(nameof(SegmentsZ));
            OnPropertyChanged(nameof(SizeX));
            OnPropertyChanged(nameof(SizeY));
            OnPropertyChanged(nameof(SizeZ));
            OnPropertyChanged(nameof(LOD));
            OnPropertyChanged(nameof(SmoothingAngle));
        }
    }

    public int SegmentsX { get; set; } = 1;
    public int SegmentsY { get; set; } = 1;
    public int SegmentsZ { get; set; } = 1;
    public float SizeX { get; set; } = 10f;
    public float SizeY { get; set; } = 0f;
    public float SizeZ { get; set; } = 10f;
    public int LOD { get; set; } = 0;
    public float SmoothingAngle { get; set; } = 178f;

    public string OutputName { get; set; } = "Primitive_Mesh";

    public IReadOnlyList<PrimitiveMeshType> AllTypes { get; } =
        System.Enum.GetValues<PrimitiveMeshType>();

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        ApplyDefaultsForType();
    }

    private void ApplyDefaultsForType()
    {
        switch (_type)
        {
            case PrimitiveMeshType.Plane:
                SegmentsX = 1; SegmentsZ = 1;
                SizeX = 10f; SizeY = 0f; SizeZ = 10f;
                break;
            case PrimitiveMeshType.Cube:
                SegmentsX = 1; SegmentsY = 1; SegmentsZ = 1;
                SizeX = 1f; SizeY = 1f; SizeZ = 1f;
                break;
            case PrimitiveMeshType.UvSphere:
                SegmentsX = 32; SegmentsY = 16;
                SizeX = 1f; SizeY = 1f; SizeZ = 1f;
                break;
            case PrimitiveMeshType.IcoSphere:
                SegmentsX = 1;
                LOD = 2;
                SizeX = 1f; SizeY = 1f; SizeZ = 1f;
                break;
            case PrimitiveMeshType.Cylinder:
                SegmentsX = 32; SegmentsY = 1; SegmentsZ = 1;
                SizeX = 1f; SizeY = 2f; SizeZ = 1f;
                break;
            case PrimitiveMeshType.Capsule:
                SegmentsX = 32; SegmentsY = 8; SegmentsZ = 1;
                SizeX = 1f; SizeY = 2f; SizeZ = 1f;
                break;
        }
    }

    public PrimitiveInitInfo BuildInfo() => new()
    {
        Type = _type,
        SegmentsX = SegmentsX,
        SegmentsY = SegmentsY,
        SegmentsZ = SegmentsZ,
        SizeX = SizeX,
        SizeY = SizeY,
        SizeZ = SizeZ,
        LOD = LOD,
        SmoothingAngle = SmoothingAngle,
    };

    public bool Create(string projectContentPath)
    {
        var info = BuildInfo();
        var fileName = $"{OutputName}.mesh";
        var fullPath = Path.Combine(projectContentPath, fileName);

        if (!NativeEngineBridge.IsLoaded)
        {
            Logger.Warning($"Primitive Mesh: native engine not loaded — saving placeholder metadata for {fileName}");
            Directory.CreateDirectory(projectContentPath);
            File.WriteAllText(fullPath, $"# PrimalEngine placeholder mesh: {info.Type}\n# segments={info.SegmentsX},{info.SegmentsY},{info.SegmentsZ}\n");
            AssetRegistry.Register(fullPath);
            return true;
        }

        var meshId = NativeEngineBridge.ProceduralMeshCreate(
            (uint)info.Type,
            new[] { info.SizeX, info.SizeY, info.SizeZ,
                    info.SegmentsX, info.SegmentsY, info.SegmentsZ,
                    info.LOD, info.SmoothingAngle });

        if (meshId == NativeEngineBridge.InvalidContentId)
        {
            Logger.Error($"PrimitiveMeshCreate failed for {info.Type}");
            return false;
        }

        try
        {
            Directory.CreateDirectory(projectContentPath);
            AssetRegistry.Register(fullPath);
            Logger.Info($"Created primitive mesh: {info.Type} -> {fullPath} (handle 0x{meshId:X16})");
            return true;
        }
        finally
        {
            NativeEngineBridge.ProceduralMeshDestroy(meshId);
        }
    }
}