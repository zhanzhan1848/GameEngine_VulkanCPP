using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Import.Settings;

public sealed class GeometryImportPanelViewModel : ImportSettingsViewModelBase
{
    private readonly GeometryImportSettings _settings;
    private bool _showAdvanced;
    private bool _pipelineAvailable = true;

    public GeometryImportPanelViewModel(IAssetImportSettings defaults)
    {
        _settings = (GeometryImportSettings)(defaults ?? new GeometryImportSettings());
    }

    public override IAssetImportSettings Settings => _settings;
    public override string ImporterName => "GeometryImporter";

    /// <summary>
    /// Show the advanced per-module pipeline knobs (PMP adaptive error,
    /// VHACD downsampling, etc.). Hidden by default because these are
    /// implementation knobs most artists don't need to touch.
    /// </summary>
    public bool ShowAdvanced
    {
        get => _showAdvanced;
        set
        {
            if (_showAdvanced == value) return;
            _showAdvanced = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(ShowAdvancedVisibility));
        }
    }
    public bool ShowAdvancedVisibility => _showAdvanced;

    /// <summary>
    /// False when the user selected a non-FBX/OBJ file (gltf/glb/dae/blend).
    /// ContentTools' pipeline path only handles .fbx and .obj; for other
    /// formats we fall back to the legacy import path and grey-out the
    /// pipeline section so toggles there do nothing misleading.
    /// </summary>
    public bool PipelineAvailable
    {
        get => _pipelineAvailable;
        set
        {
            if (_pipelineAvailable == value) return;
            _pipelineAvailable = value;
            OnPropertyChanged();
        }
    }

    public IReadOnlyList<LodGenerationMode> AvailableLodModes { get; } =
        (LodGenerationMode[])Enum.GetValues(typeof(LodGenerationMode));
    public IReadOnlyList<MeshCompressionMode> AvailableCompressions { get; } =
        (MeshCompressionMode[])Enum.GetValues(typeof(MeshCompressionMode));
    public IReadOnlyList<PipelineRemeshMode> AvailableRemeshModes { get; } =
        (PipelineRemeshMode[])Enum.GetValues(typeof(PipelineRemeshMode));
    public IReadOnlyList<PipelineSubdivideScheme> AvailableSubdivideSchemes { get; } =
        (PipelineSubdivideScheme[])Enum.GetValues(typeof(PipelineSubdivideScheme));
    public IReadOnlyList<PipelineDeriveNormalMode> AvailableDeriveNormalModes { get; } =
        (PipelineDeriveNormalMode[])Enum.GetValues(typeof(PipelineDeriveNormalMode));
    public IReadOnlyList<PipelineDeriveTangentMode> AvailableDeriveTangentModes { get; } =
        (PipelineDeriveTangentMode[])Enum.GetValues(typeof(PipelineDeriveTangentMode));
    public IReadOnlyList<PipelineUvAtlasPurpose> AvailableUvatlasPurposes { get; } =
        (PipelineUvAtlasPurpose[])Enum.GetValues(typeof(PipelineUvAtlasPurpose));

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

    public bool EnableRepair { get => _settings.EnableRepair; set { _settings.EnableRepair = value; OnPropertyChanged(); } }
    public bool EnableRemesh { get => _settings.EnableRemesh; set { _settings.EnableRemesh = value; OnPropertyChanged(); } }
    public bool EnableSubdivide { get => _settings.EnableSubdivide; set { _settings.EnableSubdivide = value; OnPropertyChanged(); } }
    public bool EnableUvatlas { get => _settings.EnableUvatlas; set { _settings.EnableUvatlas = value; OnPropertyChanged(); } }
    public bool EnableLod { get => _settings.EnableLod; set { _settings.EnableLod = value; OnPropertyChanged(); } }
    public bool EnableMeshlet { get => _settings.EnableMeshlet; set { _settings.EnableMeshlet = value; OnPropertyChanged(); } }
    public bool EnableCollision { get => _settings.EnableCollision; set { _settings.EnableCollision = value; OnPropertyChanged(); } }
    public bool EnableDerive { get => _settings.EnableDerive; set { _settings.EnableDerive = value; OnPropertyChanged(); } }
    public bool AutoDeriveAfterSubdivide { get => _settings.AutoDeriveAfterSubdivide; set { _settings.AutoDeriveAfterSubdivide = value; OnPropertyChanged(); } }
    public bool EnableSdf { get => _settings.EnableSdf; set { _settings.EnableSdf = value; OnPropertyChanged(); } }
    public uint SdfGridResolution { get => _settings.SdfGridResolution; set { _settings.SdfGridResolution = value; OnPropertyChanged(); } }

    public PipelineSubdivideScheme SubdivideScheme { get => _settings.SubdivideScheme; set { _settings.SubdivideScheme = value; OnPropertyChanged(); } }
    public uint SubdivideLevels { get => _settings.SubdivideLevels; set { _settings.SubdivideLevels = value; OnPropertyChanged(); } }

    public PipelineDeriveNormalMode DeriveNormalMode { get => _settings.DeriveNormalMode; set { _settings.DeriveNormalMode = value; OnPropertyChanged(); } }
    public PipelineDeriveTangentMode DeriveTangentMode { get => _settings.DeriveTangentMode; set { _settings.DeriveTangentMode = value; OnPropertyChanged(); } }
    public float DeriveFacetedAngleDeg { get => _settings.DeriveFacetedAngleDeg; set { _settings.DeriveFacetedAngleDeg = value; OnPropertyChanged(); } }

    public PipelineRemeshMode RemeshMode { get => _settings.RemeshMode; set { _settings.RemeshMode = value; OnPropertyChanged(); } }
    public float TargetEdgeLength { get => _settings.TargetEdgeLength; set { _settings.TargetEdgeLength = value; OnPropertyChanged(); } }
    public uint RemeshIterations { get => _settings.RemeshIterations; set { _settings.RemeshIterations = value; OnPropertyChanged(); } }
    public uint RemeshAutoSubdiv { get => _settings.RemeshAutoSubdiv; set { _settings.RemeshAutoSubdiv = value; OnPropertyChanged(); } }
    public float RemeshAdaptiveApproxError { get => _settings.RemeshAdaptiveApproxError; set { _settings.RemeshAdaptiveApproxError = value; OnPropertyChanged(); } }
    public float RemeshRatio { get => _settings.RemeshRatio; set { _settings.RemeshRatio = value; OnPropertyChanged(); } }
    public bool RemeshUseProjection { get => _settings.RemeshUseProjection; set { _settings.RemeshUseProjection = value; OnPropertyChanged(); } }

    public byte RepairOps { get => _settings.RepairOps; set { _settings.RepairOps = value; OnPropertyChanged(); } }
    public float RepairStitchDistance { get => _settings.RepairStitchDistance; set { _settings.RepairStitchDistance = value; OnPropertyChanged(); } }
    public uint RepairMaxHoleSize { get => _settings.RepairMaxHoleSize; set { _settings.RepairMaxHoleSize = value; OnPropertyChanged(); } }
    public bool RepairOrientOutward { get => _settings.RepairOrientOutward; set { _settings.RepairOrientOutward = value; OnPropertyChanged(); } }

    public PipelineUvAtlasPurpose UvatlasTargetPurpose { get => _settings.UvatlasTargetPurpose; set { _settings.UvatlasTargetPurpose = value; OnPropertyChanged(); } }
    public uint UvatlasMaxChartCount { get => _settings.UvatlasMaxChartCount; set { _settings.UvatlasMaxChartCount = value; OnPropertyChanged(); } }
    public float UvatlasGutter { get => _settings.UvatlasGutter; set { _settings.UvatlasGutter = value; OnPropertyChanged(); } }
    public bool UvatlasStretchOptimization { get => _settings.UvatlasStretchOptimization; set { _settings.UvatlasStretchOptimization = value; OnPropertyChanged(); } }
    public bool UvatlasStrictUniquePack { get => _settings.UvatlasStrictUniquePack; set { _settings.UvatlasStrictUniquePack = value; OnPropertyChanged(); } }
    public uint UvatlasResolutionHint { get => _settings.UvatlasResolutionHint; set { _settings.UvatlasResolutionHint = value; OnPropertyChanged(); } }
    public float UvatlasTexelsPerUnit { get => _settings.UvatlasTexelsPerUnit; set { _settings.UvatlasTexelsPerUnit = value; OnPropertyChanged(); } }
    public bool UvatlasUseInputUvsAsHint { get => _settings.UvatlasUseInputUvsAsHint; set { _settings.UvatlasUseInputUvsAsHint = value; OnPropertyChanged(); } }

    public float LodRatio { get => _settings.LodRatio; set { _settings.LodRatio = value; OnPropertyChanged(); } }
    public float LodTargetError { get => _settings.LodTargetError; set { _settings.LodTargetError = value; OnPropertyChanged(); } }
    public uint LodMaxLevels { get => _settings.LodMaxLevels; set { _settings.LodMaxLevels = value; OnPropertyChanged(); } }
    public bool LodLockBorders { get => _settings.LodLockBorders; set { _settings.LodLockBorders = value; OnPropertyChanged(); } }
    public bool LodOptimizeVcache { get => _settings.LodOptimizeVcache; set { _settings.LodOptimizeVcache = value; OnPropertyChanged(); } }
    public uint MinVerticesForLod { get => _settings.MinVerticesForLod; set { _settings.MinVerticesForLod = value; OnPropertyChanged(); } }

    public uint MeshletMaxVertices { get => _settings.MeshletMaxVertices; set { _settings.MeshletMaxVertices = value; OnPropertyChanged(); } }
    public uint MeshletMaxTriangles { get => _settings.MeshletMaxTriangles; set { _settings.MeshletMaxTriangles = value; OnPropertyChanged(); } }
    public float MeshletConeWeight { get => _settings.MeshletConeWeight; set { _settings.MeshletConeWeight = value; OnPropertyChanged(); } }
    public bool MeshletOptimizeVcache { get => _settings.MeshletOptimizeVcache; set { _settings.MeshletOptimizeVcache = value; OnPropertyChanged(); } }
    public bool MeshletComputeBounds { get => _settings.MeshletComputeBounds; set { _settings.MeshletComputeBounds = value; OnPropertyChanged(); } }

    public uint CollisionMaxHulls { get => _settings.CollisionMaxHulls; set { _settings.CollisionMaxHulls = value; OnPropertyChanged(); } }
    public uint CollisionVoxelResolution { get => _settings.CollisionVoxelResolution; set { _settings.CollisionVoxelResolution = value; OnPropertyChanged(); } }
    public bool CollisionAutoResolution { get => _settings.CollisionAutoResolution; set { _settings.CollisionAutoResolution = value; OnPropertyChanged(); } }
    public float CollisionMaxConcavity { get => _settings.CollisionMaxConcavity; set { _settings.CollisionMaxConcavity = value; OnPropertyChanged(); } }
    public uint CollisionMaxVerticesPerHull { get => _settings.CollisionMaxVerticesPerHull; set { _settings.CollisionMaxVerticesPerHull = value; OnPropertyChanged(); } }
    public bool CollisionPca { get => _settings.CollisionPca; set { _settings.CollisionPca = value; OnPropertyChanged(); } }
    public uint CollisionPlaneDownsampling { get => _settings.CollisionPlaneDownsampling; set { _settings.CollisionPlaneDownsampling = value; OnPropertyChanged(); } }
    public uint CollisionConvexhullDownsampling { get => _settings.CollisionConvexhullDownsampling; set { _settings.CollisionConvexhullDownsampling = value; OnPropertyChanged(); } }
}
