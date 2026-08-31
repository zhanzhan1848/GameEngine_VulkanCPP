using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using CommunityToolkit.Mvvm.ComponentModel;
using NodifyM.Avalonia.ViewModelBase;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.MaterialGraph;

namespace PrimalEditor_Avalonia.Editors.Material;

/// <summary>
/// One parameter slot on a material node (e.g. <c>ConstantFloat3.value</c>,
/// <c>SampleTexture.asset_path</c>). Edits are pushed to the native graph
/// via <see cref="MaterialGraphSession"/>; the surface just reflects the
/// value back so the UI stays the source of truth between explicit commits.
///
/// <para>
/// A single VM carries one scalar parameter regardless of its vector arity:
/// the XAML renders 1/2/3/4 <see cref="Avalonia.Controls.TextBox"/> rows
/// driven by <see cref="ShowX"/>–<see cref="ShowW"/>. Edits on a multi-
/// component param re-push the full vector through the matching
/// <see cref="MaterialGraphSession.SetParamFloat3"/> / <c>SetParamFloat4</c>
/// so the native side always sees a consistent value.
/// </para>
/// </summary>
public sealed class MaterialParamViewModel : ObservableObject
{
    public uint NodeId { get; init; }
    public string Name { get; init; } = string.Empty;
    public string Group { get; init; } = string.Empty;
    public uint Type { get; init; }
    public float Min { get; init; }
    public float Max { get; init; }
    public float Step { get; init; }

    // Component flags — driven by Type so the DataTemplate can render the
    // right number of input rows for Float / Float2 / Float3 / Float4 without
    // a DataTemplateSelector.
    public bool ShowX => IsNumeric;
    public bool ShowY => Type == (uint)MaterialParamType.Float2
                       || Type == (uint)MaterialParamType.Float3
                       || Type == (uint)MaterialParamType.Float4;
    public bool ShowZ => Type == (uint)MaterialParamType.Float3
                       || Type == (uint)MaterialParamType.Float4;
    public bool ShowW => Type == (uint)MaterialParamType.Float4;
    public bool IsNumeric => Type == (uint)MaterialParamType.Float
                          || Type == (uint)MaterialParamType.Float2
                          || Type == (uint)MaterialParamType.Float3
                          || Type == (uint)MaterialParamType.Float4;
    public bool IsTexture => Type == (uint)MaterialParamType.Texture;

    /// <summary>Imported engine textures available to material nodes.</summary>
    public IReadOnlyList<AssetInfo> TextureAssets => AssetRegistry.Assets
        .Where(asset => asset.Type == AssetType.Texture && asset.Extension == ".texture")
        .OrderBy(asset => asset.FileName, StringComparer.OrdinalIgnoreCase)
        .ToList();

    private float _x;
    public float X
    {
        get => _x;
        set { if (SetProperty(ref _x, value)) PushToNative(); }
    }
    private float _y;
    public float Y
    {
        get => _y;
        set { if (SetProperty(ref _y, value)) PushToNative(); }
    }
    private float _z;
    public float Z
    {
        get => _z;
        set { if (SetProperty(ref _z, value)) PushToNative(); }
    }
    private float _w;
    public float W
    {
        get => _w;
        set { if (SetProperty(ref _w, value)) PushToNative(); }
    }

    private string _texturePath = string.Empty;
    public string TexturePath
    {
        get => _texturePath;
        set { if (SetProperty(ref _texturePath, value ?? string.Empty)) PushTextureToNative(); }
    }

    private AssetInfo? _selectedTextureAsset;
    public AssetInfo? SelectedTextureAsset
    {
        get => _selectedTextureAsset;
        set
        {
            if (!SetProperty(ref _selectedTextureAsset, value)) return;
            var reference = value == null
                ? string.Empty
                : AssetRegistry.GetRelativePath(value.FullPath)?.Value ?? value.FullPath;
            if (SetProperty(ref _texturePath, reference, nameof(TexturePath)))
            {
                PushTextureToNative();
            }
        }
    }

    internal void SelectTextureAsset(AssetInfo asset)
    {
        // Register() can add a freshly selected .texture after this parameter
        // VM was created. Refresh the ComboBox source before selecting it.
        OnPropertyChanged(nameof(TextureAssets));
        SelectedTextureAsset = asset;
    }

    /// <summary>
    /// Backwards-compat alias for code that previously bound to a single
    /// <c>Value</c> property. Equals <see cref="X"/> for scalar params and
    /// is unused for vector params (those use X/Y/Z/W directly).
    /// </summary>
    public float Value
    {
        get => _x;
        set => X = value;
    }

    internal void InitializeNumeric(float x, float y = 0f, float z = 0f, float w = 0f)
    {
        _x = x;
        _y = y;
        _z = z;
        _w = w;
        OnPropertyChanged(nameof(X));
        OnPropertyChanged(nameof(Y));
        OnPropertyChanged(nameof(Z));
        OnPropertyChanged(nameof(W));
        OnPropertyChanged(nameof(Value));
    }

    internal void InitializeTexture(string? path)
    {
        _texturePath = path ?? string.Empty;
        _selectedTextureAsset = TextureAssets.FirstOrDefault(asset => TextureReferenceMatches(asset, _texturePath));
        OnPropertyChanged(nameof(TexturePath));
        OnPropertyChanged(nameof(SelectedTextureAsset));
    }

    private static bool TextureReferenceMatches(AssetInfo asset, string reference)
    {
        if (string.IsNullOrWhiteSpace(reference)) return false;
        if (string.Equals(Path.GetFullPath(asset.FullPath), Path.GetFullPath(reference),
                StringComparison.OrdinalIgnoreCase)) return true;

        var relative = AssetRegistry.GetRelativePath(asset.FullPath)?.Value;
        return relative != null && string.Equals(
            relative.Replace('\\', '/'), reference.Replace('\\', '/'),
            StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Push the current X/Y/Z/W values back to the native graph as the
    /// appropriate arity (Float / Float3 / Float4). Float2 reuses the v3
    /// setter with z=0 since the C ABI exposes only Float/Float3/Float4.
    /// </summary>
    private void PushToNative()
    {
        try
        {
            var session = MaterialGraphSessionLocator.Current;
            if (session == null || !session.IsValid) return;
            switch (Type)
            {
                case (uint)MaterialParamType.Float:
                    session.SetParamFloat(NodeId, Name, _x);
                    break;
                case (uint)MaterialParamType.Float2:
                    session.SetParamFloat2(NodeId, Name, _x, _y);
                    break;
                case (uint)MaterialParamType.Float3:
                    session.SetParamFloat3(NodeId, Name, _x, _y, _z);
                    break;
                case (uint)MaterialParamType.Float4:
                    session.SetParamFloat4(NodeId, Name, _x, _y, _z, _w);
                    break;
                default:
                    session.SetParamFloat(NodeId, Name, _x);
                    break;
            }
            // Refresh the right-hand preview so the user sees the change
            // immediately. Without this the native graph is updated but
            // the preview only re-renders on the next graph-topology edit
            // (add-node, connect, etc.), which was the workaround users
            // hit before. Coalesced by the preview's 30 Hz render loop.
            MaterialGraphSessionLocator.RefreshPreview();
        }
        catch (System.Exception)
        {
            // Push failures shouldn't tear down the editor; the native
            // side will report the real cause at next Execute().
        }
    }

    private void PushTextureToNative()
    {
        try
        {
            var session = MaterialGraphSessionLocator.Current;
            if (session == null || !session.IsValid) return;
            session.SetParamString(NodeId, Name, _texturePath);
            MaterialGraphSessionLocator.RefreshPreview();
        }
        catch (System.Exception) { /* swallow — see scalar branch */ }
    }
}

/// <summary>
/// Process-wide handle to the active <see cref="MaterialGraphSession"/>
/// so <see cref="MaterialParamViewModel"/> can push edits without each
/// row having to find the session. Set by
/// <see cref="MaterialEditorViewModel.EnsureGraphSession"/>; cleared by
/// <see cref="MaterialEditorViewModel.OnDispose"/>.
///
/// <para>
/// Also tracks the live <see cref="MaterialPreviewViewModel"/> so each
/// per-row commit can push a re-serialized graph to the preview pane
/// without having to round-trip through the editor VM. Without this hook
/// preview only refreshed on graph-topology edits — typing into a
/// TextBox updated native memory but left the preview stuck.
/// </para>
/// </summary>
public static class MaterialGraphSessionLocator
{
    public static MaterialGraphSession? Current { get; set; }
    public static MaterialPreviewViewModel? Preview { get; set; }

    /// <summary>
    /// Re-serialize the current graph and push it to the registered
    /// preview VM. No-op when the session or preview isn't registered
    /// (headless runs, before the editor window opens). Each TextBox
    /// edit calls this once; the preview's 30 Hz render loop coalesces
    /// multiple commits per frame.
    /// </summary>
    public static void RefreshPreview()
    {
        try
        {
            var session = Current;
            var preview = Preview;
            if (session == null || !session.IsValid || preview == null) return;
            session.Execute();
            var json = session.SerializeJson();
            if (!string.IsNullOrEmpty(json))
            {
                preview.PushGraph(PreparePreviewJson(json));
            }
        }
        catch (System.Exception)
        {
            // Preview refresh failures must not block the TextBox edit;
            // the next add-node or save round-trip will resync.
        }
    }

    /// <summary>
    /// Preview rendering happens in the native engine, which cannot infer the
    /// project Content root from a portable graph reference such as
    /// <c>Textures/Brick.texture</c>. Keep the stored graph portable, but
    /// expand ConstantTexture paths in the transient JSON sent to the preview.
    /// </summary>
    public static string PreparePreviewJson(string json)
    {
        if (string.IsNullOrWhiteSpace(json)) return json;
        var projectRoot = AssetRegistry.ProjectRoot;
        var contentRoot = AssetRegistry.ProjectContentRoot;
        if (string.IsNullOrWhiteSpace(projectRoot) && string.IsNullOrWhiteSpace(contentRoot)) return json;

        try
        {
            var root = JsonNode.Parse(json) as JsonObject;
            if (root?["nodes"] is not JsonArray nodes) return json;
            foreach (var entry in nodes.OfType<JsonObject>())
            {
                if (!string.Equals(entry["type"]?.GetValue<string>(), "ConstantTexture",
                        StringComparison.Ordinal)) continue;
                if (entry["params"] is not JsonObject parameters) continue;
                var reference = parameters["asset_path"]?.GetValue<string>();
                if (string.IsNullOrWhiteSpace(reference) || Path.IsPathRooted(reference)) continue;
                var relativePath = reference.Replace('/', Path.DirectorySeparatorChar);
                var projectCandidate = string.IsNullOrWhiteSpace(projectRoot)
                    ? null
                    : Path.GetFullPath(Path.Combine(projectRoot, relativePath));
                var contentCandidate = string.IsNullOrWhiteSpace(contentRoot)
                    ? null
                    : Path.GetFullPath(Path.Combine(contentRoot, relativePath));

                // Content Browser references are project-relative (normally
                // "Content/..."). Older graphs may still be Content-relative,
                // so preserve that format as a fallback.
                parameters["asset_path"] = projectCandidate is not null && File.Exists(projectCandidate)
                    ? projectCandidate
                    : contentCandidate ?? projectCandidate;
            }
            return root.ToJsonString(new JsonSerializerOptions { WriteIndented = false });
        }
        catch
        {
            return json;
        }
    }
}

/// <summary>
/// One node on the material graph canvas. Holds the 1-based native node id,
/// exposes the strongly-typed input/output pin collections that the custom
/// material DataTemplate binds against, and a flat parameter list for the
/// inspector panel.
///
/// <para>
/// We redeclare <c>Input</c>/<c>Output</c> on the subclass with the
/// concrete <see cref="MaterialPinViewModel"/> type instead of forwarding
/// to the base <see cref="NodeViewModelBase.Input"/>/<c>Output</c>
/// (typed as <c>ObservableCollection&lt;ConnectorViewModelBase&gt;</c>).
/// Generic collections aren't covariant so a forwarding assignment is a
/// compile error; instead our custom DataTemplate targets the strongly
/// typed members directly.
/// </para>
///
/// <para>
/// The base <see cref="NodeViewModelBase"/> (CommunityToolkit.Mvvm
/// <c>[ObservableProperty]</c>) raises <c>PropertyChanged("Location")</c>
/// every time the user drags the node. We forward that event to a
/// caller-supplied callback so the editor can mirror the canvas
/// position into the native graph (see
/// <see cref="MaterialEditorViewModel.OnNodeLocationChanged"/>) — without
/// this hook the sidecar JSON would capture the wrong x/y whenever the
/// user moves a node.
/// </para>
/// </summary>
public sealed class MaterialNodeViewModel : NodeViewModelBase
{
    public uint NodeId { get; init; }
    public string TypeName { get; init; } = string.Empty;

    public new ObservableCollection<MaterialPinViewModel> Input { get; } = new();
    public new ObservableCollection<MaterialPinViewModel> Output { get; } = new();
    public ObservableCollection<MaterialParamViewModel> Parameters { get; } = new();

    public new string Title
    {
        get => base.Title ?? TypeName;
        set => base.Title = value;
    }

    /// <summary>
    /// Optional callback invoked with the 1-based <see cref="NodeId"/> and
    /// the new canvas-space <see cref="Avalonia.Point"/> whenever the user
    /// moves this node (drag, snap, undo-redo, programmatic). Wired by
    /// <see cref="MaterialEditorViewModel.BuildNodeViewModel"/> so the
    /// native graph stays in sync with the canvas state.
    /// </summary>
    public Action<uint, Avalonia.Point>? OnLocationChanged { get; set; }

    public MaterialNodeViewModel()
    {
        PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(Location) && OnLocationChanged != null)
            {
                OnLocationChanged(NodeId, Location);
            }
        };
    }
}
