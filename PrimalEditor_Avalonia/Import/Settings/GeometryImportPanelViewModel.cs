using System;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Import.Settings;

public sealed class GeometryImportPanelViewModel : ImportSettingsViewModelBase
{
    private readonly GeometryImportSettings _settings;

    public GeometryImportPanelViewModel(IAssetImportSettings defaults)
    {
        _settings = (GeometryImportSettings)(defaults ?? new GeometryImportSettings());
    }

    public override IAssetImportSettings Settings => _settings;
    public override string ImporterName => "GeometryImporter";

    public System.Collections.Generic.IReadOnlyList<LodGenerationMode> AvailableLodModes { get; } =
        (LodGenerationMode[])Enum.GetValues(typeof(LodGenerationMode));
    public System.Collections.Generic.IReadOnlyList<MeshCompressionMode> AvailableCompressions { get; } =
        (MeshCompressionMode[])Enum.GetValues(typeof(MeshCompressionMode));

    public LodGenerationMode LodMode { get => _settings.LodMode; set { _settings.LodMode = value; OnPropertyChanged(); } }
    public float Lod0Threshold { get => _settings.Lod0Threshold; set { _settings.Lod0Threshold = value; OnPropertyChanged(); } }
    public float Lod1Threshold { get => _settings.Lod1Threshold; set { _settings.Lod1Threshold = value; OnPropertyChanged(); } }
    public float Lod2Threshold { get => _settings.Lod2Threshold; set { _settings.Lod2Threshold = value; OnPropertyChanged(); } }
    public bool GenerateTangentSpace { get => _settings.GenerateTangentSpace; set { _settings.GenerateTangentSpace = value; OnPropertyChanged(); } }
    public bool RecalculateNormals { get => _settings.RecalculateNormals; set { _settings.RecalculateNormals = value; OnPropertyChanged(); } }
    public MeshCompressionMode Compression { get => _settings.Compression; set { _settings.Compression = value; OnPropertyChanged(); } }
    public bool ForceSingleLod { get => _settings.ForceSingleLod; set { _settings.ForceSingleLod = value; OnPropertyChanged(); } }
    public float SmoothingAngle { get => _settings.SmoothingAngle; set { _settings.SmoothingAngle = value; OnPropertyChanged(); } }
    public bool CalculateNormals { get => _settings.CalculateNormals; set { _settings.CalculateNormals = value; OnPropertyChanged(); } }
    public bool CalculateTangents { get => _settings.CalculateTangents; set { _settings.CalculateTangents = value; OnPropertyChanged(); } }
    public bool ReverseHandedness { get => _settings.ReverseHandedness; set { _settings.ReverseHandedness = value; OnPropertyChanged(); } }
    public bool ImportEmbeddedTextures { get => _settings.ImportEmbeddedTextures; set { _settings.ImportEmbeddedTextures = value; OnPropertyChanged(); } }
    public bool ImportAnimations { get => _settings.ImportAnimations; set { _settings.ImportAnimations = value; OnPropertyChanged(); } }
    public bool CoalesceMeshes { get => _settings.CoalesceMeshes; set { _settings.CoalesceMeshes = value; OnPropertyChanged(); } }
}
