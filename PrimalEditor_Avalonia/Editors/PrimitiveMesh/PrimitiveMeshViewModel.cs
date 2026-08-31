using System.Collections.Generic;
using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public enum PrimitiveMeshCreationState
{
    Idle,
    Generating,
    Cooked,
    Persisted,
    Registered,
    Failed,
    Cancelled,
}

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

    private PrimitiveMeshCreationState _creationState = PrimitiveMeshCreationState.Idle;
    public PrimitiveMeshCreationState CreationState
    {
        get => _creationState;
        private set
        {
            if (_creationState != value)
            {
                _creationState = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsBusy));
                OnPropertyChanged(nameof(IsFailed));
            }
        }
    }

    public bool IsBusy => _creationState == PrimitiveMeshCreationState.Generating
                        || _creationState == PrimitiveMeshCreationState.Cooked
                        || _creationState == PrimitiveMeshCreationState.Persisted
                        || _creationState == PrimitiveMeshCreationState.Registered;

    public bool IsFailed => _creationState == PrimitiveMeshCreationState.Failed;

    public bool HasAsset => Asset != null;

    public string? LastError { get; private set; }

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        // Prefer persisted parameters from the .meta sidecar. Fall back
        // to type defaults if the sidecar is missing or corrupt.
        var loaded = AssetImportSettingsStore.Load<PrimitiveInitInfo>(info.FullPath);
        if (loaded != null && loaded.OutputName != null)
        {
            Type = loaded.Type;
            SegmentsX = loaded.SegmentsX;
            SegmentsY = loaded.SegmentsY;
            SegmentsZ = loaded.SegmentsZ;
            SizeX = loaded.SizeX;
            SizeY = loaded.SizeY;
            SizeZ = loaded.SizeZ;
            LOD = loaded.LOD;
            SmoothingAngle = loaded.SmoothingAngle;
            OutputName = loaded.OutputName;
            NotifyAllChanged();
            return;
        }
        ApplyDefaultsForType();
    }

    private void NotifyAllChanged()
    {
        OnPropertyChanged(nameof(Type));
        OnPropertyChanged(nameof(SegmentsX));
        OnPropertyChanged(nameof(SegmentsY));
        OnPropertyChanged(nameof(SegmentsZ));
        OnPropertyChanged(nameof(SizeX));
        OnPropertyChanged(nameof(SizeY));
        OnPropertyChanged(nameof(SizeZ));
        OnPropertyChanged(nameof(LOD));
        OnPropertyChanged(nameof(SmoothingAngle));
        OnPropertyChanged(nameof(OutputName));
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
        LastError = null;
        CreationState = PrimitiveMeshCreationState.Generating;
        var info = BuildInfo();
        var fileName = $"{OutputName}.mesh";
        var fullPath = Path.Combine(projectContentPath, fileName);
        ulong engineContentId = NativeEngineBridge.InvalidContentId;

        try
        {
            if (NativeEngineBridge.IsLoaded)
            {
                engineContentId = NativeEngineBridge.ProceduralMeshCreate(
                    (uint)info.Type,
                    new[] { info.SizeX, info.SizeY, info.SizeZ,
                            info.SegmentsX, info.SegmentsY, info.SegmentsZ,
                            info.LOD, info.SmoothingAngle });
            }

            CreationState = PrimitiveMeshCreationState.Cooked;
            if (!PrimitiveMeshCooker.Cook(fullPath, info))
            {
                LastError = "Cook failed: cooker returned false";
                CreationState = PrimitiveMeshCreationState.Failed;
                return false;
            }

            if (!File.Exists(fullPath))
            {
                LastError = "Cook reported success but file is missing on disk";
                CreationState = PrimitiveMeshCreationState.Failed;
                return false;
            }

            CreationState = PrimitiveMeshCreationState.Persisted;
            var registered = AssetRegistry.Register(fullPath);
            if (registered == null)
            {
                try { File.Delete(fullPath); } catch { }
                LastError = "AssetRegistry.Register returned null";
                CreationState = PrimitiveMeshCreationState.Failed;
                return false;
            }

            CreationState = PrimitiveMeshCreationState.Registered;
            // Persist the parameters used so the user can reopen the
            // asset and re-generate with the same values (or tweak and
            // re-cook via Reimport).
            AssetImportSettingsStore.Save(fullPath, info);
            Logger.Info($"Created primitive mesh: {info.Type} -> {fullPath} (engine id 0x{engineContentId:X16})");
            return true;
        }
        catch (System.Exception ex)
        {
            LastError = ex.Message;
            Logger.Error($"PrimitiveMesh.Create failed for {info.Type}", ex);
            if (File.Exists(fullPath)) { try { File.Delete(fullPath); } catch { } }
            CreationState = PrimitiveMeshCreationState.Failed;
            return false;
        }
        finally
        {
            if (engineContentId != NativeEngineBridge.InvalidContentId)
            {
                NativeEngineBridge.ProceduralMeshDestroy(engineContentId);
            }
        }
    }

    public void Cancel()
    {
        if (CreationState == PrimitiveMeshCreationState.Generating) CreationState = PrimitiveMeshCreationState.Cancelled;
    }

    /// <summary>
    /// Re-cook the .mesh file at the current asset path with the
    /// current parameter values. Persists the new params to the
    /// .meta sidecar so subsequent reopens reload them.
    /// </summary>
    public bool Reimport()
    {
        if (Asset == null)
        {
            LastError = "Reimport requires an open .mesh asset";
            return false;
        }
        var fullPath = Asset.FullPath;
        var dir = Path.GetDirectoryName(fullPath);
        if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
        {
            LastError = $"Reimport: directory missing: {dir}";
            return false;
        }
        var info = BuildInfo();
        CreationState = PrimitiveMeshCreationState.Generating;
        var previousContent = SafeBackupExistingFile(fullPath);
        try
        {
            if (!PrimitiveMeshCooker.Cook(fullPath, info))
            {
                LastError = "Reimport cook failed";
                if (previousContent != null) RestoreBackup(fullPath, previousContent);
                CreationState = PrimitiveMeshCreationState.Failed;
                return false;
            }
            AssetImportSettingsStore.Save(fullPath, info);
            // Re-registering refreshes the asset's metadata in the
            // registry (Hash, ImportDate). The path is already in
            // _byPath, so this preserves the existing GUID and only
            // updates volatile fields.
            AssetRegistry.Register(fullPath);
            Logger.Info($"Reimported primitive mesh: {fullPath} ({info.Type})");
            CreationState = PrimitiveMeshCreationState.Registered;
            return true;
        }
        finally
        {
            // No native mesh content id was created in Reimport — the
            // cooked file replaces the previous bytes in place.
        }
    }

    private static byte[]? SafeBackupExistingFile(string path)
    {
        try { return File.Exists(path) ? File.ReadAllBytes(path) : null; }
        catch { return null; }
    }

    private static void RestoreBackup(string path, byte[] backup)
    {
        try { File.WriteAllBytes(path, backup); } catch { /* best effort */ }
    }
}
