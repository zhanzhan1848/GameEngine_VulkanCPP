using System.IO;
using System.Text.Json.Nodes;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Geometry;

public enum LodGenerationMode { Manual, Auto }
public enum MeshCompressionMode { None, Auto }
public enum PipelineRemeshMode { Isotropic = 0, Adaptive = 1, Decimate = 2 }
public enum PipelineSubdivideScheme { Loop = 0, CatmullClark = 1 }
public enum PipelineDeriveNormalMode { Faceted = 0, Smooth = 1, AreaWeighted = 2, AngleWeighted = 3 }
public enum PipelineDeriveTangentMode { None = 0, AreaWeighted = 1, MikkTSpace = 2 }
public enum PipelineUvAtlasPurpose { Texture = 0, Lightmap = 1, Detail = 2 }

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

    // AssetPipeline module enable flags. Defaults match pipeline::Config
    // (LOD + Meshlet + Remesh + SDF + UV Atlas ON, others OFF) so first-time
    // imports exercise ContentTools' default processing. Override these in
    // the import dialog for any asset.
    public bool EnableRepair { get; set; } = false;
    public bool EnableRemesh { get; set; } = true;
    public bool EnableSubdivide { get; set; } = false;
    public bool EnableUvatlas { get; set; } = true;
    public bool EnableLod { get; set; } = true;
    public bool EnableMeshlet { get; set; } = true;
    public bool EnableCollision { get; set; } = false;
    public bool EnableDerive { get; set; } = false;
    public bool AutoDeriveAfterSubdivide { get; set; } = true;
    public bool EnableSdf { get; set; } = true;

    public PipelineSubdivideScheme SubdivideScheme { get; set; } = PipelineSubdivideScheme.Loop;
    public uint SubdivideLevels { get; set; } = 1;

    public PipelineDeriveNormalMode DeriveNormalMode { get; set; } = PipelineDeriveNormalMode.AreaWeighted;
    public PipelineDeriveTangentMode DeriveTangentMode { get; set; } = PipelineDeriveTangentMode.MikkTSpace;
    public float DeriveFacetedAngleDeg { get; set; } = 60f;

    public PipelineRemeshMode RemeshMode { get; set; } = PipelineRemeshMode.Isotropic;
    public float TargetEdgeLength { get; set; } = 0f;
    public uint RemeshIterations { get; set; } = 10;
    public uint RemeshAutoSubdiv { get; set; } = 1;
    public float RemeshAdaptiveApproxError { get; set; } = 0.001f;
    public float RemeshRatio { get; set; } = 0.5f;
    public bool RemeshUseProjection { get; set; } = true;

    public byte RepairOps { get; set; } = 0x0F; // repair::Op::All
    public float RepairStitchDistance { get; set; } = 1e-6f;
    public uint RepairMaxHoleSize { get; set; } = 30;
    public bool RepairOrientOutward { get; set; } = true;

    public PipelineUvAtlasPurpose UvatlasTargetPurpose { get; set; } = PipelineUvAtlasPurpose.Texture;
    public uint UvatlasMaxChartCount { get; set; } = 0;
    public float UvatlasGutter { get; set; } = 4f;
    public bool UvatlasStretchOptimization { get; set; } = true;
    public bool UvatlasStrictUniquePack { get; set; } = false;
    public uint UvatlasResolutionHint { get; set; } = 0;
    public float UvatlasTexelsPerUnit { get; set; } = 0f;
    public bool UvatlasUseInputUvsAsHint { get; set; } = true;

    public float LodRatio { get; set; } = 0.5f;
    public float LodTargetError { get; set; } = 0.01f;
    public uint LodMaxLevels { get; set; } = 6;
    public bool LodLockBorders { get; set; } = false;
    public bool LodOptimizeVcache { get; set; } = true;
    public uint MinVerticesForLod { get; set; } = 500;

    public uint MeshletMaxVertices { get; set; } = 256;
    public uint MeshletMaxTriangles { get; set; } = 128;
    public float MeshletConeWeight { get; set; } = 0.5f;
    public bool MeshletOptimizeVcache { get; set; } = true;
    public bool MeshletComputeBounds { get; set; } = true;

    public uint CollisionMaxHulls { get; set; } = 8;
    public uint CollisionVoxelResolution { get; set; } = 100000;
    public bool CollisionAutoResolution { get; set; } = true;
    public float CollisionMaxConcavity { get; set; } = 0.001f;
    public uint CollisionMaxVerticesPerHull { get; set; } = 64;
    public bool CollisionPca { get; set; } = false;
    public uint CollisionPlaneDownsampling { get; set; } = 4;
    public uint CollisionConvexhullDownsampling { get; set; } = 4;

    // ToBinary / FromBinary are FROZEN at the legacy 15-field schema
    // (Phase 1 + smoothing angle + 6 import flags). New fields are
    // JSON-only. Old .meta files keep working — FromJson tolerates missing
    // fields via the per-property TryGetPropertyValue check below.
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
        if (settings == null) return;

        // Legacy fields — preserved verbatim.
        ReadInt(settings, "LodMode", v => LodMode = (LodGenerationMode)v);
        ReadFloat(settings, "Lod0Threshold", v => Lod0Threshold = v);
        ReadFloat(settings, "Lod1Threshold", v => Lod1Threshold = v);
        ReadFloat(settings, "Lod2Threshold", v => Lod2Threshold = v);
        ReadBool(settings, "GenerateTangentSpace", v => GenerateTangentSpace = v);
        ReadBool(settings, "RecalculateNormals", v => RecalculateNormals = v);
        ReadInt(settings, "Compression", v => Compression = (MeshCompressionMode)v);
        ReadBool(settings, "ForceSingleLod", v => ForceSingleLod = v);
        ReadFloat(settings, "SmoothingAngle", v => SmoothingAngle = v);
        ReadBool(settings, "CalculateNormals", v => CalculateNormals = v);
        ReadBool(settings, "CalculateTangents", v => CalculateTangents = v);
        ReadBool(settings, "ReverseHandedness", v => ReverseHandedness = v);
        ReadBool(settings, "ImportEmbeddedTextures", v => ImportEmbeddedTextures = v);
        ReadBool(settings, "ImportAnimations", v => ImportAnimations = v);
        ReadBool(settings, "CoalesceMeshes", v => CoalesceMeshes = v);

        // Pipeline enable flags
        ReadBool(settings, "EnableRepair", v => EnableRepair = v);
        ReadBool(settings, "EnableRemesh", v => EnableRemesh = v);
        ReadBool(settings, "EnableSubdivide", v => EnableSubdivide = v);
        ReadBool(settings, "EnableUvatlas", v => EnableUvatlas = v);
        ReadBool(settings, "EnableLod", v => EnableLod = v);
        ReadBool(settings, "EnableMeshlet", v => EnableMeshlet = v);
        ReadBool(settings, "EnableCollision", v => EnableCollision = v);
        ReadBool(settings, "EnableDerive", v => EnableDerive = v);
        ReadBool(settings, "AutoDeriveAfterSubdivide", v => AutoDeriveAfterSubdivide = v);
        ReadBool(settings, "EnableSdf", v => EnableSdf = v);

        // Subdivide
        ReadInt(settings, "SubdivideScheme", v => SubdivideScheme = (PipelineSubdivideScheme)v);
        ReadUInt(settings, "SubdivideLevels", v => SubdivideLevels = v);

        // Derive
        ReadInt(settings, "DeriveNormalMode", v => DeriveNormalMode = (PipelineDeriveNormalMode)v);
        ReadInt(settings, "DeriveTangentMode", v => DeriveTangentMode = (PipelineDeriveTangentMode)v);
        ReadFloat(settings, "DeriveFacetedAngleDeg", v => DeriveFacetedAngleDeg = v);

        // Remesh
        ReadInt(settings, "RemeshMode", v => RemeshMode = (PipelineRemeshMode)v);
        ReadFloat(settings, "TargetEdgeLength", v => TargetEdgeLength = v);
        ReadUInt(settings, "RemeshIterations", v => RemeshIterations = v);
        ReadUInt(settings, "RemeshAutoSubdiv", v => RemeshAutoSubdiv = v);
        ReadFloat(settings, "RemeshAdaptiveApproxError", v => RemeshAdaptiveApproxError = v);
        ReadFloat(settings, "RemeshRatio", v => RemeshRatio = v);
        ReadBool(settings, "RemeshUseProjection", v => RemeshUseProjection = v);

        // Repair
        ReadByte(settings, "RepairOps", v => RepairOps = v);
        ReadFloat(settings, "RepairStitchDistance", v => RepairStitchDistance = v);
        ReadUInt(settings, "RepairMaxHoleSize", v => RepairMaxHoleSize = v);
        ReadBool(settings, "RepairOrientOutward", v => RepairOrientOutward = v);

        // UV Atlas
        ReadInt(settings, "UvatlasTargetPurpose", v => UvatlasTargetPurpose = (PipelineUvAtlasPurpose)v);
        ReadUInt(settings, "UvatlasMaxChartCount", v => UvatlasMaxChartCount = v);
        ReadFloat(settings, "UvatlasGutter", v => UvatlasGutter = v);
        ReadBool(settings, "UvatlasStretchOptimization", v => UvatlasStretchOptimization = v);
        ReadBool(settings, "UvatlasStrictUniquePack", v => UvatlasStrictUniquePack = v);
        ReadUInt(settings, "UvatlasResolutionHint", v => UvatlasResolutionHint = v);
        ReadFloat(settings, "UvatlasTexelsPerUnit", v => UvatlasTexelsPerUnit = v);
        ReadBool(settings, "UvatlasUseInputUvsAsHint", v => UvatlasUseInputUvsAsHint = v);

        // LOD
        ReadFloat(settings, "LodRatio", v => LodRatio = v);
        ReadFloat(settings, "LodTargetError", v => LodTargetError = v);
        ReadUInt(settings, "LodMaxLevels", v => LodMaxLevels = v);
        ReadBool(settings, "LodLockBorders", v => LodLockBorders = v);
        ReadBool(settings, "LodOptimizeVcache", v => LodOptimizeVcache = v);
        ReadUInt(settings, "MinVerticesForLod", v => MinVerticesForLod = v);

        // Meshlet
        ReadUInt(settings, "MeshletMaxVertices", v => MeshletMaxVertices = v);
        ReadUInt(settings, "MeshletMaxTriangles", v => MeshletMaxTriangles = v);
        ReadFloat(settings, "MeshletConeWeight", v => MeshletConeWeight = v);
        ReadBool(settings, "MeshletOptimizeVcache", v => MeshletOptimizeVcache = v);
        ReadBool(settings, "MeshletComputeBounds", v => MeshletComputeBounds = v);

        // Collision
        ReadUInt(settings, "CollisionMaxHulls", v => CollisionMaxHulls = v);
        ReadUInt(settings, "CollisionVoxelResolution", v => CollisionVoxelResolution = v);
        ReadBool(settings, "CollisionAutoResolution", v => CollisionAutoResolution = v);
        ReadFloat(settings, "CollisionMaxConcavity", v => CollisionMaxConcavity = v);
        ReadUInt(settings, "CollisionMaxVerticesPerHull", v => CollisionMaxVerticesPerHull = v);
        ReadBool(settings, "CollisionPca", v => CollisionPca = v);
        ReadUInt(settings, "CollisionPlaneDownsampling", v => CollisionPlaneDownsampling = v);
        ReadUInt(settings, "CollisionConvexhullDownsampling", v => CollisionConvexhullDownsampling = v);
    }

    public JsonObject ToJson()
    {
        return new JsonObject
        {
            // Legacy fields
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
            ["CoalesceMeshes"] = CoalesceMeshes,
            // Pipeline enable flags
            ["EnableRepair"] = EnableRepair,
            ["EnableRemesh"] = EnableRemesh,
            ["EnableSubdivide"] = EnableSubdivide,
            ["EnableUvatlas"] = EnableUvatlas,
            ["EnableLod"] = EnableLod,
            ["EnableMeshlet"] = EnableMeshlet,
            ["EnableCollision"] = EnableCollision,
            ["EnableDerive"] = EnableDerive,
            ["AutoDeriveAfterSubdivide"] = AutoDeriveAfterSubdivide,
            ["EnableSdf"] = EnableSdf,
            // Subdivide
            ["SubdivideScheme"] = (int)SubdivideScheme,
            ["SubdivideLevels"] = SubdivideLevels,
            // Derive
            ["DeriveNormalMode"] = (int)DeriveNormalMode,
            ["DeriveTangentMode"] = (int)DeriveTangentMode,
            ["DeriveFacetedAngleDeg"] = DeriveFacetedAngleDeg,
            // Remesh
            ["RemeshMode"] = (int)RemeshMode,
            ["TargetEdgeLength"] = TargetEdgeLength,
            ["RemeshIterations"] = RemeshIterations,
            ["RemeshAutoSubdiv"] = RemeshAutoSubdiv,
            ["RemeshAdaptiveApproxError"] = RemeshAdaptiveApproxError,
            ["RemeshRatio"] = RemeshRatio,
            ["RemeshUseProjection"] = RemeshUseProjection,
            // Repair
            ["RepairOps"] = RepairOps,
            ["RepairStitchDistance"] = RepairStitchDistance,
            ["RepairMaxHoleSize"] = RepairMaxHoleSize,
            ["RepairOrientOutward"] = RepairOrientOutward,
            // UV Atlas
            ["UvatlasTargetPurpose"] = (int)UvatlasTargetPurpose,
            ["UvatlasMaxChartCount"] = UvatlasMaxChartCount,
            ["UvatlasGutter"] = UvatlasGutter,
            ["UvatlasStretchOptimization"] = UvatlasStretchOptimization,
            ["UvatlasStrictUniquePack"] = UvatlasStrictUniquePack,
            ["UvatlasResolutionHint"] = UvatlasResolutionHint,
            ["UvatlasTexelsPerUnit"] = UvatlasTexelsPerUnit,
            ["UvatlasUseInputUvsAsHint"] = UvatlasUseInputUvsAsHint,
            // LOD
            ["LodRatio"] = LodRatio,
            ["LodTargetError"] = LodTargetError,
            ["LodMaxLevels"] = LodMaxLevels,
            ["LodLockBorders"] = LodLockBorders,
            ["LodOptimizeVcache"] = LodOptimizeVcache,
            ["MinVerticesForLod"] = MinVerticesForLod,
            // Meshlet
            ["MeshletMaxVertices"] = MeshletMaxVertices,
            ["MeshletMaxTriangles"] = MeshletMaxTriangles,
            ["MeshletConeWeight"] = MeshletConeWeight,
            ["MeshletOptimizeVcache"] = MeshletOptimizeVcache,
            ["MeshletComputeBounds"] = MeshletComputeBounds,
            // Collision
            ["CollisionMaxHulls"] = CollisionMaxHulls,
            ["CollisionVoxelResolution"] = CollisionVoxelResolution,
            ["CollisionAutoResolution"] = CollisionAutoResolution,
            ["CollisionMaxConcavity"] = CollisionMaxConcavity,
            ["CollisionMaxVerticesPerHull"] = CollisionMaxVerticesPerHull,
            ["CollisionPca"] = CollisionPca,
            ["CollisionPlaneDownsampling"] = CollisionPlaneDownsampling,
            ["CollisionConvexhullDownsampling"] = CollisionConvexhullDownsampling,
        };
    }

    /// <summary>
    /// Pack this settings instance into the flat sequential-layout
    /// <see cref="AIAssetPipelineSettings"/> for the ContentTools C ABI.
    /// Field assignment order MUST mirror the C++ struct — see
    /// AIAssetPipelineSettings.cs for the offset table.
    /// </summary>
    public AIAssetPipelineSettings ToPOD()
    {
        return new AIAssetPipelineSettings
        {
            LegacySmoothingAngle        = SmoothingAngle,
            LegacyCalculateNormals      = (byte)(CalculateNormals ? 1 : 0),
            LegacyCalculateTangents     = (byte)(CalculateTangents ? 1 : 0),
            LegacyReverseHandedness     = (byte)(ReverseHandedness ? 1 : 0),
            LegacyImportEmbeddedTextures= (byte)(ImportEmbeddedTextures ? 1 : 0),
            LegacyImportAnimations      = (byte)(ImportAnimations ? 1 : 0),
            LegacyCoalesceMeshes        = (byte)(CoalesceMeshes ? 1 : 0),

            EnableRepair               = (byte)(EnableRepair ? 1 : 0),
            EnableRemesh               = (byte)(EnableRemesh ? 1 : 0),
            EnableSubdivide            = (byte)(EnableSubdivide ? 1 : 0),
            EnableUvatlas              = (byte)(EnableUvatlas ? 1 : 0),
            EnableLod                  = (byte)(EnableLod ? 1 : 0),
            EnableMeshlet              = (byte)(EnableMeshlet ? 1 : 0),
            EnableCollision            = (byte)(EnableCollision ? 1 : 0),
            EnableDerive               = (byte)(EnableDerive ? 1 : 0),
            AutoDeriveAfterSubdivide   = (byte)(AutoDeriveAfterSubdivide ? 1 : 0),
            SubdivideScheme            = (byte)SubdivideScheme,
            DeriveNormalMode           = (byte)DeriveNormalMode,
            DeriveTangentMode          = (byte)DeriveTangentMode,

            SubdivideLevels            = SubdivideLevels,
            LodRatio                   = LodRatio,
            DeriveFacetedAngleDeg      = DeriveFacetedAngleDeg,

            // Output fields are populated by ProcessAIAsset; we leave them 0.

            TargetEdgeLength           = TargetEdgeLength,
            RemeshIterations           = RemeshIterations,
            RemeshMode                 = (byte)RemeshMode,
            RemeshAutoSubdiv           = RemeshAutoSubdiv,
            RemeshAdaptiveApproxError  = RemeshAdaptiveApproxError,
            RemeshRatio                = RemeshRatio,
            RemeshUseProjection        = (byte)(RemeshUseProjection ? 1 : 0),

            RepairOps                  = RepairOps,
            RepairStitchDistance       = RepairStitchDistance,
            RepairMaxHoleSize          = RepairMaxHoleSize,
            RepairOrientOutward        = (byte)(RepairOrientOutward ? 1 : 0),

            UvatlasTargetPurpose       = (byte)UvatlasTargetPurpose,
            UvatlasMaxChartCount       = UvatlasMaxChartCount,
            UvatlasGutter              = UvatlasGutter,
            UvatlasStretchOptimization = (byte)(UvatlasStretchOptimization ? 1 : 0),
            UvatlasStrictUniquePack    = (byte)(UvatlasStrictUniquePack ? 1 : 0),
            UvatlasResolutionHint      = UvatlasResolutionHint,
            UvatlasTexelsPerUnit       = UvatlasTexelsPerUnit,
            UvatlasUseInputUvsAsHint   = (byte)(UvatlasUseInputUvsAsHint ? 1 : 0),

            LodTargetError             = LodTargetError,
            LodMaxLevels               = LodMaxLevels,
            LodLockBorders             = (byte)(LodLockBorders ? 1 : 0),
            LodOptimizeVcache          = (byte)(LodOptimizeVcache ? 1 : 0),
            MinVerticesForLod          = MinVerticesForLod,

            MeshletMaxVertices         = MeshletMaxVertices,
            MeshletMaxTriangles        = MeshletMaxTriangles,
            MeshletConeWeight          = MeshletConeWeight,
            MeshletOptimizeVcache      = (byte)(MeshletOptimizeVcache ? 1 : 0),
            MeshletComputeBounds       = (byte)(MeshletComputeBounds ? 1 : 0),

            CollisionMaxHulls          = CollisionMaxHulls,
            CollisionVoxelResolution   = CollisionVoxelResolution,
            CollisionAutoResolution    = (byte)(CollisionAutoResolution ? 1 : 0),
            CollisionMaxConcavity      = CollisionMaxConcavity,
            CollisionMaxVerticesPerHull= CollisionMaxVerticesPerHull,
            CollisionPca               = (byte)(CollisionPca ? 1 : 0),
            CollisionPlaneDownsampling = CollisionPlaneDownsampling,
            CollisionConvexhullDownsampling = CollisionConvexhullDownsampling,

            EnableSdf                  = (byte)(EnableSdf ? 1 : 0),
        };
    }

    private static void ReadInt(JsonObject obj, string key, System.Action<int> setter)
    {
        if (obj.TryGetPropertyValue(key, out var node) && node is JsonValue v && v.TryGetValue(out int i))
            setter(i);
    }
    private static void ReadUInt(JsonObject obj, string key, System.Action<uint> setter)
    {
        if (obj.TryGetPropertyValue(key, out var node) && node is JsonValue v && v.TryGetValue(out uint u))
            setter(u);
    }
    private static void ReadFloat(JsonObject obj, string key, System.Action<float> setter)
    {
        if (obj.TryGetPropertyValue(key, out var node) && node is JsonValue v && v.TryGetValue(out float f))
            setter(f);
    }
    private static void ReadBool(JsonObject obj, string key, System.Action<bool> setter)
    {
        if (obj.TryGetPropertyValue(key, out var node) && node is JsonValue v && v.TryGetValue(out bool b))
            setter(b);
    }
    private static void ReadByte(JsonObject obj, string key, System.Action<byte> setter)
    {
        if (obj.TryGetPropertyValue(key, out var node) && node is JsonValue v && v.TryGetValue(out byte b))
            setter(b);
    }
}
