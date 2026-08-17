using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using MeshGeometry = PrimalEditor_Avalonia.Editors.Geometry.Geometry;

namespace PrimalEditor_Avalonia.Editors.MeshAssetInspector;

/// <summary>
/// Asset inspector for mesh assets (.model.json, .model, .mesh).
/// Builds a two-branch tree (Meshes / LODs) from the cooked .mesh and
/// its optional sidecar, and exposes read-only inspector sections
/// (General / Selected Submesh / Active LOD) that update when the user
/// picks a different node or LOD index.
///
/// Mesh parsing is delegated to <see cref="Editors.Geometry.Geometry"/>
/// — the same parser the wireframe preview pane in the Content
/// Browser uses — so the two surfaces stay in sync.
/// </summary>
public sealed class MeshAssetInspectorViewModel : AssetInspectorViewModel
{
    private readonly MeshGeometry _geometry = new();
    private List<ModelSubmeshEntry> _sidecarEntries = new();
    private IReadOnlyList<AssetNode> _roots = Array.Empty<AssetNode>();
    private int _activeLodIndex;

    public MeshAssetInspectorViewModel(AssetInfo asset) : base(asset)
    {
        Reload();
    }

    public MeshGeometry Geometry => _geometry;

    public int ActiveLodIndex
    {
        get => _activeLodIndex;
        set
        {
            var clamped = Math.Clamp(value, 0, Math.Max(0, _geometry.LODs.Count - 1));
            if (_activeLodIndex == clamped) return;
            _activeLodIndex = clamped;
            OnPropertyChanged();
            OnPropertyChanged(nameof(InspectorSections));
        }
    }

    public int SubmeshCount => _geometry.LODs.Count > 0 ? _geometry.LODs[0].Meshes.Count : 0;
    public int LodCount => _geometry.LODs.Count;

    public IReadOnlyList<int> LodIndices
    {
        get
        {
            var list = new List<int>(_geometry.LODs.Count);
            for (var i = 0; i < _geometry.LODs.Count; i++) list.Add(i);
            return list;
        }
    }

    public override IReadOnlyList<AssetNode> TreeRoots => _roots;

    public override IReadOnlyList<InspectorSection> InspectorSections
    {
        get
        {
            var sections = new List<InspectorSection>
            {
                new("General", new[]
                {
                    new InspectorRow("Name", Asset?.FileName ?? "—"),
                    new InspectorRow("Format", Path.GetExtension(Asset?.FullPath ?? "").TrimStart('.').ToUpperInvariant()),
                    new InspectorRow("Path", Asset?.FullPath ?? "—"),
                    new InspectorRow("Size", Asset?.SizeDisplay ?? "—"),
                    new InspectorRow("LODs", LodCount.ToString()),
                    new InspectorRow("Submeshes", SubmeshCount.ToString()),
                }),
            };
            if (_selectedKind == AssetNodeKind.Submesh && _selectedSubmeshIndex >= 0)
                sections.Add(BuildSubmeshSection());
            else if (_selectedKind == AssetNodeKind.LOD)
                sections.Add(BuildLodSection());
            return sections;
        }
    }

    public override IReadOnlyList<IPreviewAction> PreviewActions { get; } = new IPreviewAction[]
    {
        new CopyGuidAction(),
        new RevealInFileManagerAction(),
    };

    public override bool IsDraggable(AssetNode node)
        => node.Kind is AssetNodeKind.Model or AssetNodeKind.Submesh or AssetNodeKind.LOD;

    private AssetNodeKind _selectedKind = AssetNodeKind.Model;
    private int _selectedSubmeshIndex = -1;

    protected override void OnNodeSelected(AssetNode? node)
    {
        if (node == null)
        {
            _selectedKind = AssetNodeKind.Model;
            _selectedSubmeshIndex = -1;
        }
        else
        {
            _selectedKind = node.Kind;
            _selectedSubmeshIndex = node.Kind == AssetNodeKind.Submesh ? node.Index : -1;
        }
        OnPropertyChanged(nameof(InspectorSections));
    }

    private void Reload()
    {
        var meshPath = ResolveCookedMeshPath(Asset);
        if (meshPath == null || !File.Exists(meshPath))
        {
            StatusMessage = "Cooked .mesh not found — re-import required";
            _roots = Array.Empty<AssetNode>();
            OnPropertyChanged(nameof(TreeRoots));
            OnPropertyChanged(nameof(InspectorSections));
            return;
        }
        if (!_geometry.Load(meshPath))
        {
            StatusMessage = "Could not parse .mesh binary";
            return;
        }

        _sidecarEntries = Asset?.Type == AssetType.Model
            ? (ModelSidecar.Load(Asset.FullPath)?.Submeshes
                .OrderBy(s => s.MeshIndex)
                .ToList() ?? new List<ModelSubmeshEntry>())
            : new List<ModelSubmeshEntry>();

        var submeshNames = _sidecarEntries.Count > 0
            ? _sidecarEntries.Select(e => string.IsNullOrEmpty(e.Name) ? $"Submesh {e.MeshIndex}" : e.Name).ToList()
            : null;

        var meshNodes = new List<AssetNode>(_geometry.LODs[0].Meshes.Count);
        for (var i = 0; i < _geometry.LODs[0].Meshes.Count; i++)
        {
            var name = submeshNames != null && i < submeshNames.Count
                ? submeshNames[i]
                : $"Submesh {i}";
            meshNodes.Add(new AssetNode(AssetNodeKind.Submesh, name, i, IntPtr.Zero));
        }
        var lodNodes = new List<AssetNode>(_geometry.LODs.Count);
        for (var i = 0; i < _geometry.LODs.Count; i++)
        {
            lodNodes.Add(new AssetNode(AssetNodeKind.LOD, $"LOD{i}", i, IntPtr.Zero));
        }
        _roots = new[]
        {
            new AssetNode(AssetNodeKind.RootNode, Asset?.FileNameWithoutExt ?? "model", -1, IntPtr.Zero,
                new[]
                {
                    new AssetNode(AssetNodeKind.MeshesFolder, "Meshes", -1, IntPtr.Zero, meshNodes),
                    new AssetNode(AssetNodeKind.LODsFolder, "LODs", -1, IntPtr.Zero, lodNodes),
                }),
        };
        StatusMessage = $"{meshNodes.Count} submeshes, {lodNodes.Count} LODs";
        OnPropertyChanged(nameof(TreeRoots));
        OnPropertyChanged(nameof(SubmeshCount));
        OnPropertyChanged(nameof(LodCount));
        OnPropertyChanged(nameof(LodIndices));
        OnPropertyChanged(nameof(InspectorSections));
    }

    private static string? ResolveCookedMeshPath(AssetInfo? info)
    {
        if (info == null) return null;
        if (info.Type == AssetType.Model)
        {
            var sidecar = ModelSidecar.Load(info.FullPath);
            if (sidecar != null && !string.IsNullOrEmpty(sidecar.Mesh) && File.Exists(sidecar.Mesh))
                return sidecar.Mesh;
            var sibling = Path.ChangeExtension(info.FullPath, ".mesh");
            return File.Exists(sibling) ? sibling : null;
        }
        if (info.Type == AssetType.Mesh)
        {
            var sibling = Path.ChangeExtension(info.FullPath, ".mesh");
            return File.Exists(sibling) ? sibling : info.FullPath;
        }
        return null;
    }

    private InspectorSection BuildSubmeshSection()
    {
        var idx = _selectedSubmeshIndex;
        var lod = _geometry.LODs[Math.Clamp(_activeLodIndex, 0, _geometry.LODs.Count - 1)];
        var mesh = idx < lod.Meshes.Count ? lod.Meshes[idx] : null;
        var entry = idx < _sidecarEntries.Count ? _sidecarEntries[idx] : null;
        return new("Submesh", new[]
        {
            new InspectorRow("Index", idx.ToString()),
            new InspectorRow("Vertices", mesh?.VertexCount.ToString() ?? "0"),
            new InspectorRow("Indices", mesh?.IndexCount.ToString() ?? "0"),
            new InspectorRow("Material", entry?.MaterialIndex.ToString() ?? "—"),
            new InspectorRow("Source LOD", entry?.LodIndex.ToString() ?? "—"),
        });
    }

    private InspectorSection BuildLodSection()
    {
        var lod = _geometry.LODs[Math.Clamp(_activeLodIndex, 0, _geometry.LODs.Count - 1)];
        var verts = lod.Meshes.Sum(m => (long)m.VertexCount);
        var idx = lod.Meshes.Sum(m => (long)m.IndexCount);
        return new("Active LOD", new[]
        {
            new InspectorRow("Index", _activeLodIndex.ToString()),
            new InspectorRow("Submeshes", lod.Meshes.Count.ToString()),
            new InspectorRow("Vertices", verts.ToString()),
            new InspectorRow("Indices", idx.ToString()),
        });
    }

    private sealed class CopyGuidAction : IPreviewAction
    {
        public string Label => "Copy GUID";
        public void Execute(AssetInspectorViewModel inspector)
        {
            var clipboard = Avalonia.Application.Current?.ApplicationLifetime
                is Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
                ? desktop.MainWindow?.Clipboard
                : null;
            if (clipboard != null && inspector.Asset != null)
            {
                Avalonia.Threading.Dispatcher.UIThread.Post(async () =>
                    await clipboard.SetTextAsync(inspector.Asset.Guid.ToString()));
            }
        }
    }

    private sealed class RevealInFileManagerAction : IPreviewAction
    {
        public string Label => "Reveal";
        public void Execute(AssetInspectorViewModel inspector)
        {
            var path = inspector.Asset?.FullPath;
            if (string.IsNullOrEmpty(path)) return;
            try
            {
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
                {
                    FileName = path,
                    UseShellExecute = true,
                });
            }
            catch (Exception ex)
            {
                Logger.Warning($"Reveal failed: {ex.Message}");
            }
        }
    }
}