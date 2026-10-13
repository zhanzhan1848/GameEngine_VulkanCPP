using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Import;
using PrimalEditor_Avalonia.Import.Settings;
using PrimalEditor_Avalonia.Native.MaterialGraph;

namespace PrimalEditor_Avalonia.Native.Importers;

public sealed class MeshImporterPlugin : IAssetImporterPlugin
{
    private const int MeshPollTimeoutMs = 60_000;
    private const int MeshPollIntervalMs = 50;

    private static readonly HashSet<string> _exts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".fbx", ".obj", ".gltf", ".glb", ".dae", ".blend"
    };

    static MeshImporterPlugin()
    {
        AssetImporterRegistry.Register(new MeshImporterPlugin());
    }

    public string DisplayName => "Model Importer";
    public IReadOnlyList<AssetType> HandledTypes { get; } = new[] { AssetType.Mesh };
    public Type SettingsType => typeof(GeometryImportSettings);
    public bool CanClaim(string filePath) => _exts.Contains(Path.GetExtension(filePath));
    public UserControl CreatePanel(IAssetImportSettings defaults)
    {
        var vm = new GeometryImportPanelViewModel(defaults);
        var panel = new GeometryImportPanel { DataContext = vm };
        return panel;
    }

    public async Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct)
    {
        // Source file (e.g. .fbx / .obj / .gltf) is NOT copied into the
        // project; the content browser shows the model via its cooked
        // .mesh + .model.json sidecar. The source path is recorded in
        // the sidecar's Source field for traceability and re-cooking.
        Directory.CreateDirectory(request.DestinationDir);

        var sourceStem = Path.GetFileNameWithoutExtension(request.SourcePath);

        // Per-model asset folder: everything this import produces (cooked
        // .mesh, .model.json sidecar, default material + graph, cooked
        // .texture assets) lives under "<DestinationDir>/<stem>/" so the
        // asset is self-contained and moves/deletes as one unit in the
        // content browser.
        var assetDir = Path.Combine(request.DestinationDir, sourceStem);
        var cookedPath = Path.Combine(assetDir, sourceStem + ".mesh");
        var sidecarPath = Path.Combine(assetDir, sourceStem + ".model.json");

        var settings = (GeometryImportSettings)request.Settings;

        if (!NativeEngineBridge.IsLoaded)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = sidecarPath,
                Type = AssetType.Model,
                Success = false,
                ErrorMessage = "ContentTools not loaded; cannot cook mesh.",
            };
        }

        // Validate before any filesystem side effects beyond the
        // destination root: a failed import must not leave folders behind.
        if (File.Exists(assetDir))
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = request.DestinationDir,
                Type = AssetType.Model,
                Success = false,
                ErrorMessage = $"Cannot create asset folder '{sourceStem}': a file with that name already exists.",
            };
        }
        Directory.CreateDirectory(assetDir);

        var pod = settings.ToPOD();
        var cookOk = await Task.Run(() => NativeEngineBridge.CookMeshWithPipeline(request.SourcePath, cookedPath, pod), ct);
        if (!cookOk)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = sidecarPath,
                Type = AssetType.Model,
                Success = false,
                ErrorMessage = "ContentTools cooking failed",
            };
        }

        if (pod.ValidationErrors > 0 || pod.ValidationWarnings > 0)
        {
            Logger.Warning(
                $"Mesh import: {Path.GetFileName(request.SourcePath)} cooked with " +
                $"{pod.ValidationErrors} validation errors and {pod.ValidationWarnings} warnings " +
                $"(meshes={pod.MeshCount}, meshlets={pod.MeshletCount}).");
        }

        // The cooked .mesh material header references textures by the paths
        // recorded in the SOURCE asset (bare filenames or source-relative
        // paths). Cook every referenced file into the engine's native
        // .texture (TEXR) format under "<stem>_textures/" next to the
        // cooked mesh — matching the TextureImporterPlugin convention — so
        // the default material graph can reference them and the world
        // renderer can bind them (PipelineImportCookedTexture consumes
        // cooked TEXR only). Missing files are logged, not fatal.
        // .gltf/.glb embedded images never hit this path as missing: the
        // C++ glTF importer extracts them to "<stem>_extracted/" beside the
        // source (absolute temp paths when the source dir is read-only) and
        // records those references in the sidecar material records.
        var cookedTextures = CookReferencedTextures(request.SourcePath, assetDir, sourceStem, sidecarPath);

        // Create a per-model default material whose graph references the
        // cooked textures, so the imported asset renders textured once an
        // instance is placed (placement auto-assigns this sibling material;
        // the world renderer binds the albedo/normal/ORM triple from the
        // first entry of MeshRendererComponent.MaterialGuids).
        CreateDefaultMaterial(assetDir, sourceStem, sidecarPath, cookedTextures);

        // The .model.json sidecar is already written by ContentTools'
        // CookMeshWithPipeline (ContentTools.cpp:762) using real per-mesh
        // metadata. We only need to mint the asset GUID + import settings
        // here; the sidecar itself is the cooker's responsibility.

        // Save .meta against the sidecar (not the cooked .mesh): the
        // registry registers the sidecar as the user-visible asset, and
        // .mesh is a hidden cooked artifact. Keeping .meta with the
        // user-facing file prevents GUID drift if the mesh is regenerated.
        AssetImportSettingsStore.Save<GeometryImportSettings>(sidecarPath, settings);

        var requestId = NativeEngineBridge.ImportMesh(cookedPath);
        if (requestId == 0)
        {
            return new ImportedAsset
            {
                SourcePath = request.SourcePath,
                DestinationPath = sidecarPath,
                Type = AssetType.Model,
                Success = false,
                ErrorMessage = "Mesh loader rejected the request (job system down?)",
            };
        }

        var deadline = DateTime.UtcNow.AddMilliseconds(MeshPollTimeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            ct.ThrowIfCancellationRequested();
            NativeEngineBridge.TickMainThreadJobs();
            var (meshId, status) = NativeEngineBridge.TryGetMeshImportResult(requestId);
            if (status != NativeEngineBridge.MeshImportResultStatus.Pending)
            {
                return new ImportedAsset
                {
                    SourcePath = request.SourcePath,
                    DestinationPath = sidecarPath,
                    Type = AssetType.Model,
                    Success = status == NativeEngineBridge.MeshImportResultStatus.Success,
                    NativeHandle = meshId,
                    ErrorMessage = status switch
                    {
                        NativeEngineBridge.MeshImportResultStatus.Failed => "Engine create_resource() returned invalid_id",
                        NativeEngineBridge.MeshImportResultStatus.NotFound => "Internal: request id lost (engine reloaded?)",
                        _ => null,
                    },
                };
            }
            await Task.Delay(MeshPollIntervalMs, ct);
        }

        return new ImportedAsset
        {
            SourcePath = request.SourcePath,
            DestinationPath = sidecarPath,
            Type = AssetType.Model,
            Success = false,
            ErrorMessage = $"Mesh import timed out after {MeshPollTimeoutMs / 1000}s",
        };
    }

    // Directory (relative to the mesh) that receives the cooked .texture
    // assets for this model. ForwardSceneRenderer additionally probes this
    // subdir for RAW texture files when loading EngineTest-style assets.
    internal static string TextureSubdir(string sourceStem) => sourceStem + "_textures";

    // Cooks every texture referenced by the sidecar's material records into
    // the engine's native .texture (TEXR) format under "<stem>_textures/".
    // Returns a map from the raw reference string to the cooked file path
    // (first role wins when a file is referenced by multiple slots).
    private static IReadOnlyDictionary<string, string> CookReferencedTextures(
        string sourcePath, string destinationDir, string sourceStem, string sidecarPath)
    {
        var cookedByRef = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        var sidecar = ModelSidecar.Load(sidecarPath);
        if (sidecar == null || sidecar.Materials.Count == 0)
        {
            Logger.Debug($"Mesh import: no texture references in sidecar for '{sourceStem}'; skipping texture cook.");
            return cookedByRef;
        }

        var sourceDir = Path.GetDirectoryName(Path.GetFullPath(sourcePath));
        if (string.IsNullOrEmpty(sourceDir))
        {
            Logger.Warning($"Mesh import: cannot resolve source directory for texture cook of '{sourceStem}'.");
            return cookedByRef;
        }

        // Compressed cooks shell out to astcenc; App startup normally exports
        // it, but re-point the env defensively (idempotent, cheap) so a cook
        // never fails for a missing tool.
        AstcencLocator.EnsureExported();

        // Albedo keeps the sRGB BC7 defaults; normals are linear BC5 like
        // the texture importer's normal-map preset.
        var albedoSettings = new TextureImportSettings();
        var normalSettings = new TextureImportSettings
        {
            OutputFormat = TextureFormat.BC5_UNORM,
            IsSRGB = false,
            IsNormalMap = true,
            PreferBC7 = false,
        };

        var pending = new List<(string Reference, bool IsNormal)>();
        foreach (var mat in sidecar.Materials)
        {
            if (TextureRefUsable(mat.Diffuse)) pending.Add((NormalizeRef(mat.Diffuse), false));
            if (TextureRefUsable(mat.Normal)) pending.Add((NormalizeRef(mat.Normal), true));
        }
        if (pending.Count == 0)
        {
            Logger.Debug($"Mesh import: no usable texture references for '{sourceStem}'; skipping texture cook.");
            return cookedByRef;
        }

        var textureRoot = Path.Combine(destinationDir, TextureSubdir(sourceStem));
        int cooked = 0, missing = 0, failed = 0;
        foreach (var (reference, isNormal) in pending)
        {
            if (cookedByRef.ContainsKey(reference)) continue; // dedupe; first role wins

            var sourceFile = FindTextureFile(sourceDir, reference);
            if (sourceFile == null)
            {
                ++missing;
                Logger.Warning($"Mesh import: texture '{reference}' referenced by '{sourceStem}' not found under {sourceDir}.");
                continue;
            }

            var destPath = CookedTexturePath(textureRoot, reference);
            if (!File.Exists(destPath))
            {
                var settings = isNormal ? normalSettings : albedoSettings;
                var (payload, status, nativeError) = NativeEngineBridge.CookTexture(sourceFile, settings);
                if (payload == null)
                {
                    ++failed;
                    Logger.Warning($"Mesh import: CookTexture failed for '{reference}' (status={status}, nativeError={nativeError}).");
                    continue;
                }
                Directory.CreateDirectory(textureRoot);
                TextureTexrWriter.Write(destPath, payload);
            }

            cookedByRef[reference] = destPath;
            ++cooked;
        }

        Logger.Info($"Mesh import: textures for '{sourceStem}' — cooked {cooked}, missing {missing}, failed {failed} → {textureRoot}");
        return cookedByRef;
    }

    // Sibling default-material convention ("<stem>.material.json" next to
    // the .model.json sidecar). WorldEditorViewModel's drop handler looks
    // this file up to auto-assign the material on placement.
    internal static string DefaultMaterialPath(string destinationDir, string sourceStem) =>
        Path.Combine(destinationDir, sourceStem + ".material.json");

    internal static void CreateDefaultMaterial(
        string destinationDir, string sourceStem, string sidecarPath,
        IReadOnlyDictionary<string, string> cookedTextures)
    {
        var materialPath = DefaultMaterialPath(destinationDir, sourceStem);
        if (File.Exists(materialPath))
        {
            // Re-import must not clobber a material the user may already
            // have edited in the material editor.
            // 派生件补齐（asset-pipeline S1）：材质保留，但材质图缺失时（首次
            // 导入时引擎未加载被跳过/被误删）按“现有材质”解析的纹理三元组
            // 重建图——绝不按导入期首选材质覆盖用户改过的绑定。
            var missingGraph = Path.ChangeExtension(materialPath, ".material.graph.json");
            if (!File.Exists(missingGraph) && NativeEngineBridge.IsLoaded)
                TryRecoverGraphFromExistingMaterial(materialPath, missingGraph, sourceStem);
            Logger.Debug($"Mesh import: default material '{Path.GetFileName(materialPath)}' already exists; keeping it.");
            return;
        }

        var sidecar = ModelSidecar.Load(sidecarPath);
        var primary = sidecar == null ? null : PickPrimaryMaterial(sidecar);
        if (primary == null)
        {
            Logger.Debug($"Mesh import: no material records for '{sourceStem}'; skipping default material.");
            return;
        }

        string? albedo = null, normal = null;
        if (!string.IsNullOrWhiteSpace(primary.Diffuse)) cookedTextures.TryGetValue(NormalizeRef(primary.Diffuse), out albedo);
        if (!string.IsNullOrWhiteSpace(primary.Normal)) cookedTextures.TryGetValue(NormalizeRef(primary.Normal), out normal);
        if (albedo == null)
        {
            Logger.Warning($"Mesh import: diffuse texture '{primary.Diffuse}' was not cooked for '{sourceStem}'; skipping default material.");
            return;
        }

        MaterialSerializer.WriteNew(materialPath, sourceStem);
        AssetRegistry.Register(materialPath);

        if (!NativeEngineBridge.IsLoaded)
        {
            Logger.Info($"Mesh import: default material '{Path.GetFileName(materialPath)}' created; graph sidecar skipped (engine not loaded).");
            return;
        }

        try
        {
            using var session = MaterialGraphSession.Create();
            var output = session.AddNode("MaterialOutput");
            session.SetNodePosition(output, 360f, 0f);
            ConnectTextureNode(session, output, outputPin: 4, albedo, y: -96f);
            if (normal != null) ConnectTextureNode(session, output, outputPin: 5, normal, y: 96f);
            session.Execute();
            var json = output != 0 ? session.SerializeJson() : null;
            if (json == null)
            {
                Logger.Warning($"Mesh import: material graph build failed for '{sourceStem}' (material file kept, no graph).");
                return;
            }
            var graphPath = Path.ChangeExtension(materialPath, ".material.graph.json");
            File.WriteAllText(graphPath, json);
            AssetRegistry.Register(graphPath);
            Logger.Info($"Mesh import: default material '{Path.GetFileName(materialPath)}' created with textured graph (albedo{(normal != null ? " + normal" : "")}).");
        }
        catch (Exception ex)
        {
            Logger.Warning($"Mesh import: material graph creation failed for '{sourceStem}': {ex.Message}");
        }
    }

    /// <summary>
    /// 重导入补齐缺失的材质图（asset-pipeline S1）：以<b>现有</b>材质文件解析
    /// 出的纹理三元组重建图，不读 sidecar 首选材质——用户在材质编辑器改过的
    /// 绑定因此被尊重。全空材质（无纹理）只记 Debug 不建图。失败非致命：材质
    /// 原样保留，下次重导入再试。
    /// </summary>
    private static void TryRecoverGraphFromExistingMaterial(
        string materialPath, string graphPath, string sourceStem)
    {
        try
        {
            var slots = MaterialWorldTextureResolver.Resolve(materialPath);
            var albedo = slots.Length > 0 && !string.IsNullOrWhiteSpace(slots[0]) ? slots[0] : null;
            var normal = slots.Length > 1 && !string.IsNullOrWhiteSpace(slots[1]) ? slots[1] : null;
            if (albedo == null)
            {
                Logger.Debug($"Mesh import: existing material '{Path.GetFileName(materialPath)}' has no textures — graph not recovered.");
                return;
            }
            using var session = MaterialGraphSession.Create();
            var output = session.AddNode("MaterialOutput");
            session.SetNodePosition(output, 360f, 0f);
            ConnectTextureNode(session, output, outputPin: 4, albedo, y: -96f);
            if (normal != null) ConnectTextureNode(session, output, outputPin: 5, normal, y: 96f);
            session.Execute();
            var json = session.SerializeJson();
            if (json == null)
            {
                Logger.Warning($"Mesh import: graph recovery for '{sourceStem}' failed (serialize) — material kept as-is.");
                return;
            }
            File.WriteAllText(graphPath, json);
            AssetRegistry.Register(graphPath);
            Logger.Info($"Mesh import: recovered missing material graph for '{Path.GetFileName(materialPath)}' from its current textures.");
        }
        catch (Exception ex)
        {
            Logger.Warning($"Mesh import: graph recovery failed for '{sourceStem}': {ex.Message} — material kept as-is.");
        }
    }

    // The engine session needs the engine loaded (native graph slot); callers
    // must check NativeEngineBridge.IsLoaded first.
    private static void ConnectTextureNode(
        MaterialGraphSession session, uint outputNode, uint outputPin, string texturePath, float y)
    {
        var textureNode = session.AddNode("ConstantTexture");
        session.SetNodePosition(textureNode, -120f, y);
        var projectRelative = AssetRegistry.GetRelativePath(texturePath)?.Value;
        session.SetParamString(textureNode, "asset_path",
            projectRelative ?? texturePath);
        session.Connect(textureNode, 0, outputNode, outputPin);
    }

    // "<stem>_textures/Textures/colormap.png"-style reference → flattened
    // cooked asset name ("Textures_colormap.texture").
    private static string CookedTexturePath(string textureRoot, string reference)
    {
        var flattened = reference.Replace('\\', '/').Replace('/', '_');
        return Path.Combine(textureRoot,
            Path.GetFileNameWithoutExtension(flattened) + TextureImporterPlugin.EngineAssetExtension);
    }

    private static bool TextureRefUsable(string? reference)
    {
        if (string.IsNullOrWhiteSpace(reference)) return false;
        // Embedded glTF textures ("*0") carry no usable path.
        return !string.IsNullOrEmpty(Path.GetExtension(reference));
    }

    // Canonical key for texture references: trimmed, forward slashes, no
    // URI query. Used for the cook map, the cooked file name, and source
    // lookup alike.
    private static string NormalizeRef(string reference)
    {
        var clean = reference.Trim().Replace('\\', '/');
        var queryStart = clean.IndexOf('?');
        return queryStart >= 0 ? clean[..queryStart] : clean;
    }

    // The world renderer binds one texture triple per entity, so the
    // default material picks the material record the most submeshes use.
    private static ModelMaterialEntry? PickPrimaryMaterial(ModelSidecar sidecar)
    {
        if (sidecar.Materials.Count == 0) return null;
        var bestIndex = 0u;
        var bestCount = -1;
        if (sidecar.Submeshes.Count > 0)
        {
            var counts = new Dictionary<uint, int>();
            foreach (var sub in sidecar.Submeshes)
                counts[sub.MaterialIndex] = counts.TryGetValue(sub.MaterialIndex, out var c) ? c + 1 : 1;
            foreach (var (index, count) in counts)
            {
                if (count <= bestCount || index >= sidecar.Materials.Count) continue;
                bestCount = count;
                bestIndex = index;
            }
        }
        return sidecar.Materials[(int)bestIndex];
    }

    // Resolves a texture reference from the source asset: first as a
    // source-relative path, then by filename anywhere under the source
    // directory (FBX only records relative filenames; glTF URIs may be
    // URL-encoded). Bounded so a huge tree cannot stall the import.
    private static string? FindTextureFile(string sourceDir, string reference)
    {
        var clean = Uri.UnescapeDataString(reference.Replace('\\', '/'));
        var queryStart = clean.IndexOf('?');
        if (queryStart >= 0) clean = clean[..queryStart];

        var direct = Path.Combine(sourceDir, clean);
        if (File.Exists(direct)) return direct;

        var fileName = Path.GetFileName(clean);
        return SearchFileBfs(sourceDir, fileName, maxDepth: 4, maxEntries: 4000);
    }

    private static string? SearchFileBfs(string root, string fileName, int maxDepth, int maxEntries)
    {
        var queue = new Queue<(string dir, int depth)>();
        queue.Enqueue((root, 0));
        var visited = 0;
        while (queue.Count > 0 && visited < maxEntries)
        {
            var (dir, depth) = queue.Dequeue();
            string[] files;
            try { files = Directory.GetFiles(dir); }
            catch (Exception) { continue; }
            foreach (var file in files)
            {
                if (++visited > maxEntries) return null;
                if (string.Equals(Path.GetFileName(file), fileName, StringComparison.OrdinalIgnoreCase))
                    return file;
            }
            if (depth >= maxDepth) continue;
            string[] subDirs;
            try { subDirs = Directory.GetDirectories(dir); }
            catch (Exception) { continue; }
            foreach (var sub in subDirs)
            {
                var name = Path.GetFileName(sub);
                if (name.StartsWith('.')) continue; // skip .git etc.
                queue.Enqueue((sub, depth + 1));
            }
        }
        return null;
    }
}
