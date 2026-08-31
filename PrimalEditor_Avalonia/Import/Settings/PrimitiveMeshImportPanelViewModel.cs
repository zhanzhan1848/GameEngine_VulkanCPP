using System;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.PrimitiveMesh;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Import.Settings;

public sealed class PrimitiveMeshImportPanelViewModel : ImportSettingsViewModelBase
{
    private readonly PrimitiveInitInfo _settings;

    public PrimitiveMeshImportPanelViewModel(IAssetImportSettings defaults)
    {
        _settings = (PrimitiveInitInfo)(defaults ?? new PrimitiveInitInfo());
    }

    public override IAssetImportSettings Settings => _settings;
    public override string ImporterName => "PrimitiveMeshImporter";

    public System.Collections.Generic.IReadOnlyList<PrimitiveMeshType> AvailableTypes { get; } =
        (PrimitiveMeshType[])Enum.GetValues(typeof(PrimitiveMeshType));

    public PrimitiveMeshType Type { get => _settings.Type; set { _settings.Type = value; OnPropertyChanged(); } }
    public int SegmentsX { get => _settings.SegmentsX; set { _settings.SegmentsX = value; OnPropertyChanged(); } }
    public int SegmentsY { get => _settings.SegmentsY; set { _settings.SegmentsY = value; OnPropertyChanged(); } }
    public int SegmentsZ { get => _settings.SegmentsZ; set { _settings.SegmentsZ = value; OnPropertyChanged(); } }
    public float SizeX { get => _settings.SizeX; set { _settings.SizeX = value; OnPropertyChanged(); } }
    public float SizeY { get => _settings.SizeY; set { _settings.SizeY = value; OnPropertyChanged(); } }
    public float SizeZ { get => _settings.SizeZ; set { _settings.SizeZ = value; OnPropertyChanged(); } }
    public int LOD { get => _settings.LOD; set { _settings.LOD = value; OnPropertyChanged(); } }
    public float SmoothingAngle { get => _settings.SmoothingAngle; set { _settings.SmoothingAngle = value; OnPropertyChanged(); } }
    public string OutputName { get => _settings.OutputName; set { _settings.OutputName = value; OnPropertyChanged(); } }
}
