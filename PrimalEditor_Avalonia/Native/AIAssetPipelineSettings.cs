using System;
using System.Runtime.InteropServices;

namespace PrimalEditor_Avalonia.Native;

/// <summary>
/// Sequential-layout C# mirror of <c>ai_asset_pipeline_params</c> in
/// <c>ContentTools/ContentTools.cpp</c>. Field order MUST match the C++
/// struct exactly — the ABI is matched by offset, not name.
///
/// Sentinel defaults: a field value of 0 (or 0.0f) means "use the
/// pipeline::Config module default for this field". The C# side sends 0
/// for fields the user didn't touch, and a positive value to override.
///
/// This struct is intentionally flat — the C++ POD is also flat. We do
/// NOT use nested structs or marshal-as-by-value IntPtr fields, because
/// alignment padding between them is a constant source of ABI mismatch
/// bugs.
///
/// Total size: 208 bytes on the target ABI (8-byte aligned). Asserted at
/// startup by <see cref="AssertSize"/>.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct AIAssetPipelineSettings
{
    // -- Legacy geo_settings (FBX/OBJ parser-side) --
    public float LegacySmoothingAngle;
    public byte LegacyCalculateNormals;
    public byte LegacyCalculateTangents;
    public byte LegacyReverseHandedness;
    public byte LegacyImportEmbeddedTextures;
    public byte LegacyImportAnimations;
    public byte LegacyCoalesceMeshes;

    // -- I/O (set by CookMeshWithPipeline) --
    public IntPtr InputPath;
    public IntPtr OutData;

    // -- Pipeline enable flags --
    public byte EnableRepair;
    public byte EnableRemesh;
    public byte EnableSubdivide;
    public byte EnableUvatlas;
    public byte EnableLod;
    public byte EnableMeshlet;
    public byte EnableCollision;
    public byte EnableDerive;
    public byte AutoDeriveAfterSubdivide;
    public byte SubdivideScheme;    // 0=Loop, 1=CatmullClark
    public byte DeriveNormalMode;   // 0=Faceted, 1=Smooth, 2=AreaWeighted, 3=AngleWeighted
    public byte DeriveTangentMode;  // 0=None, 1=AreaWeighted, 2=MikkTSpace

    public uint SubdivideLevels;
    public float LodRatio;
    public float DeriveFacetedAngleDeg;

    // -- Output (filled by ProcessAIAsset, read by caller) --
    public uint MeshCount;
    public uint MeshletCount;
    public uint ValidationErrors;
    public uint ValidationWarnings;

    // -- Remesh --
    public float TargetEdgeLength;
    public uint RemeshIterations;
    public byte RemeshMode;                 // 0=Isotropic, 1=Adaptive, 2=Decimate
    public uint RemeshAutoSubdiv;
    public float RemeshAdaptiveApproxError;
    public float RemeshRatio;
    public byte RemeshUseProjection;

    // -- Repair --
    public byte RepairOps;                  // bitfield of repair::Op
    public float RepairStitchDistance;
    public uint RepairMaxHoleSize;
    public byte RepairOrientOutward;

    // -- UV Atlas --
    public byte UvatlasTargetPurpose;       // 0=Texture, 1=Lightmap, 2=Detail
    public uint UvatlasMaxChartCount;
    public float UvatlasGutter;
    public byte UvatlasStretchOptimization;
    public byte UvatlasStrictUniquePack;
    public uint UvatlasResolutionHint;
    public float UvatlasTexelsPerUnit;
    public byte UvatlasUseInputUvsAsHint;

    // -- LOD --
    public float LodTargetError;
    public uint LodMaxLevels;
    public byte LodLockBorders;
    public byte LodOptimizeVcache;
    public uint MinVerticesForLod;

    // -- Meshlet --
    public uint MeshletMaxVertices;
    public uint MeshletMaxTriangles;
    public float MeshletConeWeight;
    public byte MeshletOptimizeVcache;
    public byte MeshletComputeBounds;

    // -- Collision --
    public uint CollisionMaxHulls;
    public uint CollisionVoxelResolution;
    public byte CollisionAutoResolution;
    public float CollisionMaxConcavity;
    public uint CollisionMaxVerticesPerHull;
    public byte CollisionPca;
    public uint CollisionPlaneDownsampling;
    public uint CollisionConvexhullDownsampling;

    // -- SDF --
    public byte EnableSdf;

    /// <summary>
    /// Asserts <see cref="Marshal.SizeOf{T}"/> matches the C++ struct size.
    /// A mismatch means the C# mirror has drifted from the C++ POD and
    /// every ABI call will silently mis-parse — this is the only thing
    /// that catches a field-order typo.
    /// </summary>
    public const int ExpectedSizeBytes = 208;

    public static void AssertSize()
    {
        int actual = Marshal.SizeOf<AIAssetPipelineSettings>();
        if (actual != ExpectedSizeBytes)
        {
            throw new InvalidOperationException(
                $"AIAssetPipelineSettings ABI size mismatch: C# is {actual} bytes, " +
                $"ContentTools.cpp POD is {ExpectedSizeBytes} bytes. " +
                "Field order or types drifted — do NOT run the editor until fixed.");
        }
    }
}
