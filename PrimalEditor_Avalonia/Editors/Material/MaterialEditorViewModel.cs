using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Windows.Input;
using CommunityToolkit.Mvvm.ComponentModel;
using NodifyM.Avalonia.Events;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.MaterialGraph;

namespace PrimalEditor_Avalonia.Editors.Material;

public class MaterialEditorViewModel : AssetEditorViewModel, IDirtyTrackable
{
    public MaterialDocument Document { get; private set; } = new();

    public MaterialEditorViewModel()
    {
        // NodifyM 12.0.0's ConnectionCompletedCommand is invoked with a
        // (SourceConnector, TargetConnector) ValueTuple —
        // NOT a PendingConnectionEventArgs. Using a strongly-typed
        // RelayCommand<PendingConnectionEventArgs> here throws
        // TargetInvocationException every time a connection drag
        // completes (RelayCommand<>.CanExecute rejects the tuple argument
        // as the wrong type). We use ConnectionCompletedRelayCommand
        // (defined below) which accepts any object parameter and does a
        // runtime type switch.
        ConnectionCompletedCommand = new ConnectionCompletedRelayCommand(HandleConnectionCompletedPayload);

        // The right-hand preview pane owns its own VM so the EditorVM
        // stays focused on graph state. Always constructed eagerly so the
        // preview bitmap exists before the View binds; the VM no-ops if
        // the engine dylib is not loaded.
        Preview = new MaterialPreviewViewModel();
    }

    /// <summary>
    /// 3D preview pane VM. Lives for the editor's lifetime; the View
    /// calls <see cref="MaterialPreviewViewModel.Start"/> /
    /// <see cref="MaterialPreviewViewModel.Stop"/> on Loaded / Unloaded.
    /// </summary>
    public MaterialPreviewViewModel Preview { get; }

    private void HandleConnectionCompletedPayload(object? parameter)
    {
        if (!TryGetConnectionPins(parameter, out var src, out var dst)) return;
        Connect(src, dst);
    }

    /// <summary>
    /// Extracts connector VMs from every payload shape used by NodifyM.
    /// ValueTuple is invariant, so a boxed
    /// <c>ValueTuple&lt;MaterialPinViewModel, MaterialPinViewModel&gt;</c> does
    /// not match <c>ValueTuple&lt;object, object&gt;</c>.  ITuple handles both
    /// the concrete and base-connector generic forms without reflection.
    /// </summary>
    internal static bool TryGetConnectionPins(
        object? parameter,
        out MaterialPinViewModel source,
        out MaterialPinViewModel target)
    {
        object? sourceObject = null;
        object? targetObject = null;

        if (parameter is PendingConnectionEventArgs pending)
        {
            sourceObject = pending.SourceConnector;
            targetObject = pending.TargetConnector;
        }
        else if (parameter is ITuple tuple && tuple.Length >= 2)
        {
            sourceObject = tuple[0];
            targetObject = tuple[1];
        }

        source = sourceObject as MaterialPinViewModel ?? null!;
        target = targetObject as MaterialPinViewModel ?? null!;
        return source != null && target != null;
    }

    public string Name
    {
        get => Document.Name;
        set
        {
            if (Document.Name == value) return;
            Document.Name = value ?? string.Empty;
            OnPropertyChanged();
            MarkDirty();
        }
    }

    public AssetGuid ShaderGuid
    {
        get => Document.ShaderGuid;
        set
        {
            if (Document.ShaderGuid == value) return;
            Document.ShaderGuid = value;
            OnPropertyChanged();
            MarkDirty();
        }
    }

    public IDictionary<string, string> TextureSlots
    {
        get => _textureSlotView;
        private set { _textureSlotView = value; OnPropertyChanged(); }
    }
    private IDictionary<string, string> _textureSlotView = new Dictionary<string, string>();

    public string Surface
    {
        get => Document.RenderState.Surface;
        set
        {
            if (Document.RenderState.Surface == value) return;
            Document.RenderState.Surface = value;
            OnPropertyChanged();
            MarkDirty();
        }
    }

    public string CullMode
    {
        get => Document.RenderState.CullMode;
        set
        {
            if (Document.RenderState.CullMode == value) return;
            Document.RenderState.CullMode = value;
            OnPropertyChanged();
            MarkDirty();
        }
    }

    public bool DepthTest
    {
        get => Document.RenderState.DepthTest;
        set { Document.RenderState.DepthTest = value; OnPropertyChanged(); MarkDirty(); }
    }

    public bool DepthWrite
    {
        get => Document.RenderState.DepthWrite;
        set { Document.RenderState.DepthWrite = value; OnPropertyChanged(); MarkDirty(); }
    }

    private bool _isDirty;
    public new bool IsDirty
    {
        get => _isDirty;
        private set { if (_isDirty != value) { _isDirty = value; OnPropertyChanged(); OnPropertyChanged(nameof(DirtyMarker)); } }
    }
    public override void MarkDirty() => IsDirty = true;
    public override void ClearDirty() => IsDirty = false;

    /// <summary>
    /// Visible <c>*</c> indicator next to the filename in the toolbar
    /// when the editor has unsaved changes. Bound from
    /// <see cref="Editors.Material.MaterialEditorView.axaml"/>.
    /// </summary>
    public string DirtyMarker => _isDirty ? "● unsaved" : string.Empty;

    /// <summary>
    /// Bottom status-line text. Shows the latest graph-execute error or
    /// "Ready" when clean. Bound from
    /// <see cref="Editors.Material.MaterialEditorView.axaml"/>.
    /// </summary>
    public string StatusLine
    {
        get => _statusLine;
        private set { if (_statusLine != value) { _statusLine = value; OnPropertyChanged(); } }
    }
    private string _statusLine = "Ready";

    // ============================================================
    //  Node-graph surface (Phase 5)
    // ============================================================

    /// <summary>
    /// Native <see cref="MaterialGraphSession"/> backing the canvas.
    /// Lazy-created on first access; null when the engine dylib isn't
    /// loaded (e.g. running tests in a clean CI host).
    /// </summary>
    public MaterialGraphSession? GraphSession { get; private set; }

    public ObservableCollection<MaterialNodeViewModel> Nodes { get; } = new();
    public ObservableCollection<MaterialConnectionViewModel> Connections { get; } = new();

    public IReadOnlyList<MaterialNodeTemplateViewModel> NodePalette { get; private set; } =
        Array.Empty<MaterialNodeTemplateViewModel>();

    private MaterialNodeViewModel? _selectedNode;
    public MaterialNodeViewModel? SelectedNode
    {
        get => _selectedNode;
        set
        {
            if (ReferenceEquals(_selectedNode, value)) return;
            _selectedNode = value;
            OnPropertyChanged();
        }
    }

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        try
        {
            Document = MaterialSerializer.Read(info.FullPath) ?? new MaterialDocument
            {
                Name = Path.GetFileNameWithoutExtension(info.FileName),
            };
        }
        catch (System.Exception ex)
        {
            Logger.Error($"MaterialEditorViewModel.SetAsset read failed: {ex.Message}\n{ex.StackTrace}", ex);
            Document = new MaterialDocument { Name = Path.GetFileNameWithoutExtension(info.FileName) };
        }
        try { SyncTextureSlots(); }
        catch (System.Exception ex) { Logger.Error($"SetAsset SyncTextureSlots: {ex.Message}\n{ex.StackTrace}", ex); }
        try { ReloadGraphFromSidecar(); }
        catch (System.Exception ex) { Logger.Error($"SetAsset ReloadGraphFromSidecar: {ex.Message}\n{ex.StackTrace}", ex); }
        try { EnsureDefaultOutputNode(); }
        catch (System.Exception ex) { Logger.Error($"SetAsset EnsureDefaultOutputNode: {ex.Message}\n{ex.StackTrace}", ex); }
        ClearDirty();
    }

    /// <summary>
    /// First-open convenience: every material graph needs a <c>MaterialOutput</c>
    /// terminal, and an empty canvas looks broken to the user. If neither the
    /// sidecar nor an in-memory graph produced a node, drop a single
    /// <c>MaterialOutput</c> at the canvas origin so the editor opens with
    /// something visibly connected. Safe to call repeatedly; the
    /// <see cref="MaterialGraphSession.AddNode"/> call returns 0 if the
    /// native type isn't registered and we silently skip in that case.
    /// </summary>
    private void EnsureDefaultOutputNode()
    {
        if (Nodes.Count > 0) return;
        var output = NodePalette.FirstOrDefault(t =>
            t.IsTerminal || t.TypeName.Equals("MaterialOutput", StringComparison.OrdinalIgnoreCase));
        if (output == null) return;
        AddNode(output);
    }

    public override void Save()
    {
        if (Asset == null) return;
        try
        {
            MaterialSerializer.Write(Asset.FullPath, Document);
            SaveGraphToSidecar();
            ClearDirty();
            StatusLine = $"Saved {Asset.FileName}";
            Logger.Info($"Saved material: {Asset.FileName}");
            // The scene runtime caches per-GUID graph handles loaded at sync
            // time; without this push the entities already rendering the
            // material keep the pre-save topology until an app restart.
            if (Asset.Guid != Guid.Empty)
                SceneRuntimeSynchronizer.NotifyMaterialSaved(new AssetGuid(Asset.Guid));
        }
        catch (Exception ex)
        {
            // Surface the failure to the bottom status bar — the button
            // otherwise looks like a no-op when the write throws.
            StatusLine = $"Save failed: {ex.Message}";
            Logger.Error($"Material save failed: {ex.Message}", ex);
        }
    }

    public override void Discard()
    {
        if (Asset == null) return;
        SetAsset(Asset);
        ClearDirty();
    }

    /// <summary>
    /// Tear down owned resources when the editor window is being disposed.
    /// Currently this just disposes the right-hand preview VM so the native
    /// <c>MaterialPreviewRenderer</c> slot in the C++ registry is released
    /// promptly. Subclasses (and other callers) can chain their own cleanup
    /// on top.
    /// </summary>
    public virtual void OnDispose()
    {
        try { Preview?.Dispose(); }
        catch (Exception ex) { Logger.Warning($"OnDispose Preview.Dispose: {ex.Message}"); }
        // Drop the locator references so a closed editor doesn't leak
        // into a fresh open — RefreshPreview and graph-session lookups
        // both go through this locator.
        if (ReferenceEquals(MaterialGraphSessionLocator.Current, GraphSession))
        {
            MaterialGraphSessionLocator.Current = null;
        }
        MaterialGraphSessionLocator.Preview = null;
    }

    /// <summary>
    /// Forward the current graph JSON to the right-hand preview pane so
    /// it can re-render. Called from <see cref="AddNode"/>,
    /// <see cref="Connect"/>, <see cref="Save"/>, and
    /// <see cref="ReloadGraphFromSidecar"/>. Param edits already push their
    /// values via <see cref="MaterialGraphSession.SetParamFloat"/>, but the
    /// preview needs the full graph (topology + values), so we reserialize
    /// and push the JSON here. Cheap (one Serialize round-trip per edit).
    /// </summary>
    private void PushGraphToPreview()
    {
        try
        {
            if (GraphSession == null) return;
            GraphSession.Execute();
            var json = GraphSession.SerializeJson();
            if (!string.IsNullOrEmpty(json))
            {
                Preview.PushGraph(PreviewPushJson(json));
            }
        }
        catch (Exception ex)
        {
            Logger.Warning($"PushGraphToPreview: {ex.Message}");
        }
    }

    public void SyncTextureSlots()
    {
        TextureSlots = Document.Textures.ToDictionary(kv => kv.Key, kv => kv.Value.Value.ToString("D"));
    }

    // --- Graph operations ---

    /// <summary>
    /// Lazy-init the graph session and load the node-type palette. Called
    /// once per editor lifetime; subsequent calls are no-ops. Also wires
    /// the <see cref="MaterialGraphSessionLocator.Current"/> so parameter
    /// edits on nodes can push values back to the native graph without
    /// each row having to find the session.
    /// </summary>
    private void EnsureGraphSession()
    {
        if (GraphSession != null) return;
        if (!Native.NativeEngineBridge.IsLoaded)
        {
            Logger.Warning("MaterialEditorViewModel: engine not loaded, graph editing unavailable");
            return;
        }

        GraphSession = MaterialGraphSession.Create();
        MaterialGraphSessionLocator.Current = GraphSession;
        MaterialGraphSessionLocator.Preview = Preview;
        NodePalette = LoadNodePalette();
        OnPropertyChanged(nameof(NodePalette));
    }

    private static IReadOnlyList<MaterialNodeTemplateViewModel> LoadNodePalette()
    {
        var list = new List<MaterialNodeTemplateViewModel>();
        var count = MaterialGraphReflectionNative.GetRegisteredTypeCount();
        for (uint i = 0; i < count; i++)
        {
            if (MaterialGraphReflectionNative.TryGetRegisteredTypeInfo(
                    i, 64, 64, 64, out var typeName, out var display, out var category, out var terminal))
            {
                list.Add(new MaterialNodeTemplateViewModel
                {
                    TypeName = typeName,
                    DisplayName = display,
                    Category = category,
                    IsTerminal = terminal,
                });
            }
        }
        return list;
    }

    /// <summary>Add a node from the palette by <see cref="MaterialNodeTemplateViewModel.TypeName"/>.</summary>
    public MaterialNodeViewModel? AddNode(MaterialNodeTemplateViewModel template)
    {
        var vm = AddNodeInternal(template);
        if (vm != null)
        {
            // Cascade-position by node count so consecutive adds don't
            // pile up at (0,0). The canvas is in canvas coordinates.
            vm.Location = new Avalonia.Point(40 + Nodes.Count * 32, 40 + Nodes.Count * 24);
            // Mirror the cascade position into the native graph so the
            // sidecar JSON captures the layout even if the user closes
            // the editor without dragging the node.
            OnNodeLocationChanged(vm.NodeId, vm.Location);
        }
        return vm;
    }

    /// <summary>Create a ConstantTexture node at the asset drop point and assign the asset.</summary>
    public MaterialNodeViewModel? AddTextureAssetAt(AssetInfo asset, Avalonia.Point location)
    {
        if (asset.Type != AssetType.Texture || !asset.Extension.Equals(".texture", StringComparison.OrdinalIgnoreCase))
            return null;
        var template = NodePalette.FirstOrDefault(t =>
            t.TypeName.Equals("ConstantTexture", StringComparison.OrdinalIgnoreCase));
        if (template == null) return null;
        var node = AddNodeAt(template, location);
        if (node != null) AssignTextureAsset(node, asset);
        return node;
    }

    /// <summary>Assign a Content Browser texture to an existing ConstantTexture node.</summary>
    public bool AssignTextureAsset(MaterialNodeViewModel node, AssetInfo asset)
    {
        if (!node.TypeName.Equals("ConstantTexture", StringComparison.OrdinalIgnoreCase) ||
            asset.Type != AssetType.Texture ||
            !asset.Extension.Equals(".texture", StringComparison.OrdinalIgnoreCase)) return false;
        var parameter = node.Parameters.FirstOrDefault(p => p.IsTexture);
        if (parameter == null) return false;
        parameter.SelectTextureAsset(asset);
        SelectedNode = node;
        MarkDirty();
        PushGraphToPreview();
        return true;
    }

    /// <summary>
    /// Drop variant of <see cref="AddNode"/> — places the new node at the
    /// exact drop location on the canvas. Used by the palette drag-and-drop
    /// handler in <c>MaterialEditorView.OnCanvasDrop</c>.
    /// </summary>
    public MaterialNodeViewModel? AddNodeAt(MaterialNodeTemplateViewModel template, Avalonia.Point canvasPosition)
    {
        var vm = AddNodeInternal(template);
        if (vm != null)
        {
            vm.Location = canvasPosition;
            OnNodeLocationChanged(vm.NodeId, canvasPosition);
        }
        return vm;
    }

    private MaterialNodeViewModel? AddNodeInternal(MaterialNodeTemplateViewModel template)
    {
        EnsureGraphSession();
        if (GraphSession == null) return null;
        var id = GraphSession.AddNode(template.TypeName);
        if (id == 0)
        {
            Logger.Warning($"AddNode: native graph rejected type '{template.TypeName}'");
            return null;
        }

        var vm = BuildNodeViewModel(id, template.TypeName);
        if (vm != null)
        {
            vm.Title = template.DisplayName;
            Nodes.Add(vm);
            MarkDirty();
            PushGraphToPreview();
        }
        return vm;
    }

    /// <summary>
    /// Per-node hook called when the editor's
    /// <see cref="NodifyM.Avalonia.ViewModelBase.NodeViewModelBase.Location"/>
    /// changes (user dropped the node, drag-pan finalized, etc.). Mirrors
    /// the canvas-space (x, y) into the native graph so the next
    /// <see cref="MaterialGraphSession.SaveToFile"/> round-trips the
    /// layout. Cheap (one P/Invoke per finished move) and idempotent.
    /// </summary>
    public void OnNodeLocationChanged(uint nodeId, Avalonia.Point location)
    {
        if (GraphSession?.IsValid != true) return;
        try
        {
            GraphSession.SetNodePosition(nodeId, (float)location.X, (float)location.Y);
        }
        catch (Exception ex)
        {
            Logger.Warning($"OnNodeLocationChanged: {ex.Message}");
        }
    }

    /// <summary>RelayCommand wrapper around <see cref="Connect(MaterialPinViewModel, MaterialPinViewModel)"/>
    /// for NodifyEditor's <c>ConnectionCompletedCommand</c> binding.
    /// The argument is the framework's <see cref="PendingConnectionEventArgs"/>.</summary>
    public ICommand ConnectionCompletedCommand { get; }

    /// <summary>Connect two pin view-models by calling the native bridge.</summary>
    public bool Connect(MaterialPinViewModel source, MaterialPinViewModel target)
    {
        if (GraphSession == null || source == null || target == null) return false;
        // NodifyM semantics: drag from an output to an input. The source/target
        // types are already known by the connector — but we still re-check
        // because the user can in principle start a connection from either side.
        var from = source.IsInput ? target : source;
        var to   = source.IsInput ? source : target;
        if (from.IsInput || !to.IsInput) return false;
        if (!GraphSession.Connect(from.NodeId, from.PinIndex, to.NodeId, to.PinIndex)) return false;

        // Native Connect replaces an existing edge targeting this input.
        // Mirror that replacement locally without calling Disconnect: doing
        // so after Connect could remove the newly accepted edge as well.
        var replaced = Connections
            .Where(c => c.ToNodeId == to.NodeId && c.ToPin == to.PinIndex)
            .ToList();
        foreach (var old in replaced)
        {
            Connections.Remove(old);
        }
        Connections.Add(new MaterialConnectionViewModel(EditorViewModelPlaceholder.Instance, this, from, to));
        MarkDirty();
        PushGraphToPreview();
        return true;
    }

    /// <summary>
    /// Remove a connection from the in-memory VM collection and from the
    /// native graph. Called from <see cref="MaterialConnectionViewModel.DisconnectCommand"/>
    /// when the user clicks the ✕ button on a connection. No-op when the
    /// native graph no longer knows about the link (already gone, e.g. on
    /// reload), so the VM-side removal still runs and the canvas cleans up.
    /// </summary>
    public void RemoveConnection(MaterialConnectionViewModel connection)
    {
        if (connection == null) return;
        Connections.Remove(connection);
        try
        {
            if (GraphSession != null && GraphSession.IsValid)
            {
                GraphSession.Disconnect(
                    connection.FromNodeId, connection.FromPin,
                    connection.ToNodeId,   connection.ToPin);
            }
        }
        catch (Exception ex)
        {
            Logger.Warning($"RemoveConnection: native Disconnect failed: {ex.Message}");
        }
        MarkDirty();
        PushGraphToPreview();
    }

    /// <summary>
    /// Delete the selected node from both the native graph and the canvas.
    /// Native node ids are vector indices and therefore shift after erase,
    /// so the complete VM projection is rebuilt from the native session.
    /// </summary>
    public bool RemoveSelectedNode()
    {
        var node = SelectedNode;
        if (node == null || GraphSession?.IsValid != true) return false;

        try
        {
            GraphSession.RemoveNode(node.NodeId);
            SelectedNode = null;
            RebuildFromSession();
            MarkDirty();
            PushGraphToPreview();
            return true;
        }
        catch (Exception ex)
        {
            Logger.Warning($"RemoveSelectedNode: {ex.Message}");
            return false;
        }
    }

    private MaterialNodeViewModel? BuildNodeViewModel(uint nodeId, string typeName)
    {
        if (GraphSession == null) return null;

        var node = new MaterialNodeViewModel { NodeId = nodeId, TypeName = typeName };
        // Wire VM → native position mirror BEFORE we touch Location so the
        // initial read-back doesn't fire OnLocationChanged (no-op when both
        // sides agree) and every subsequent drag pushes through.
        node.OnLocationChanged = OnNodeLocationChanged;
        var pinCount = GraphSession.GetPinCount(nodeId);
        for (uint i = 0; i < pinCount; i++)
        {
            if (!GraphSession.TryGetPin(nodeId, i, out var desc, out var name)) continue;
            var pin = new MaterialPinViewModel
            {
                NodeId = nodeId,
                // Descriptor order and native pin index differ on nodes that
                // have both inputs and outputs. Each direction has its own
                // zero-based native index space.
                PinIndex = desc.Index,
                DataType = desc.DataType,
                IsInput = desc.IsInput != 0,
                // Flow drives which side of the node the connector renders on; must
                // match IsInput so NodifyM places the visual on the right edge.
                Flow = desc.IsInput != 0
                    ? NodifyM.Avalonia.ViewModelBase.ConnectorViewModelBase.ConnectorFlow.Input
                    : NodifyM.Avalonia.ViewModelBase.ConnectorViewModelBase.ConnectorFlow.Output,
                Title = name,
            };
            if (desc.IsInput != 0) node.Input.Add(pin);
            else node.Output.Add(pin);
        }
        PopulateNodeParameters(node);
        // Restore the canvas position from the native graph so reloaded
        // nodes appear exactly where the user last left them. TryGetNodePosition
        // returns false for nodes that were never positioned (legacy
        // sidecars created before position serialization shipped); in that
        // case we leave the VM at (0,0) and the caller is expected to
        // cascade-position later (see AddNode).
        if (GraphSession.TryGetNodePosition(nodeId, out var px, out var py))
        {
            node.Location = new Avalonia.Point(px, py);
        }
        return node;
    }

    /// <summary>
    /// Query the native graph for the parameters exposed by
    /// <paramref name="node"/> and populate the inspector panel's
    /// <see cref="MaterialNodeViewModel.Parameters"/> collection. The
    /// current value is read from native memory; binding edits push them
    /// back through <see cref="MaterialGraphSession.SetParamFloat"/>
    /// (and its float3 / float4 / string siblings).
    /// </summary>
    private void PopulateNodeParameters(MaterialNodeViewModel node)
    {
        if (GraphSession == null) return;
        var count = MaterialGraphReflectionNative.GetParamCount(GraphSession.GraphId, node.NodeId);
        for (uint i = 0; i < count; i++)
        {
            if (!MaterialGraphReflectionNative.TryGetParamDescriptor(
                    GraphSession.GraphId, node.NodeId, i, 64, 64,
                    out var desc, out var name, out var group)) continue;

            var p = new MaterialParamViewModel
            {
                NodeId = node.NodeId,
                Name = name,
                Group = group,
                Type = desc.Type,
                Min = desc.MinVal,
                Max = desc.MaxVal,
                Step = desc.Step == 0 ? 0.01f : desc.Step,
            };

            // Read the native value into the right component(s). Float2
            // shares the v4 ABI with z/w zeroed; Texture reads the asset
            // path; everything else falls back to a scalar Float read.
            try
            {
                switch (desc.Type)
                {
                    case (uint)MaterialParamType.Float:
                    {
                        GraphSession.TryGetParamFloat(node.NodeId, name, out var f);
                        p.InitializeNumeric(f);
                        break;
                    }
                    case (uint)MaterialParamType.Float2:
                    case (uint)MaterialParamType.Float3:
                    case (uint)MaterialParamType.Float4:
                    {
                        if (GraphSession.TryGetParamFloat4(node.NodeId, name, out var v))
                        {
                            p.InitializeNumeric(
                                v.Length > 0 ? v[0] : 0f,
                                v.Length > 1 ? v[1] : 0f,
                                v.Length > 2 ? v[2] : 0f,
                                v.Length > 3 ? v[3] : 0f);
                        }
                        break;
                    }
                    case (uint)MaterialParamType.Texture:
                    {
                        if (GraphSession.TryGetParamString(node.NodeId, name, out var path))
                            p.InitializeTexture(path);
                        break;
                    }
                    default:
                    {
                        GraphSession.TryGetParamFloat(node.NodeId, name, out var f);
                        p.InitializeNumeric(f);
                        break;
                    }
                }
            }
            catch { /* leave defaults; next edit round-trips through SetParam* */ }

            node.Parameters.Add(p);
        }
    }

    /// <summary>Rebuild the view-model collections from the current <see cref="GraphSession"/> state.</summary>
    public void RebuildFromSession()
    {
        Nodes.Clear();
        Connections.Clear();
        if (GraphSession == null) return;

        var indexById = new Dictionary<uint, MaterialNodeViewModel>();
        var nodeCount = GraphSession.NodeCount;
        for (uint i = 0; i < nodeCount; i++)
        {
            var id = GraphSession.GetNodeIdByIndex(i);
            var typeName = GraphSession.GetNodeTypeName(id) ?? "Node";
            var node = BuildNodeViewModel(id, typeName);
            if (node == null) continue;
            var palette = NodePalette.FirstOrDefault(p => p.TypeName == typeName);
            if (palette != null) node.Title = palette.DisplayName;
            indexById[id] = node;
            Nodes.Add(node);
        }

        foreach (var c in GraphSession.Connections)
        {
            if (!indexById.TryGetValue(c.FromNode, out var fromNode) ||
                !indexById.TryGetValue(c.ToNode,   out var toNode)) continue;
            if (c.FromPin >= fromNode.Output.Count || c.ToPin >= toNode.Input.Count) continue;
            var src = fromNode.Output[(int)c.FromPin];
            var dst = toNode.Input[(int)c.ToPin];
            Connections.Add(new MaterialConnectionViewModel(EditorViewModelPlaceholder.Instance, this, src, dst));
        }
    }

    // --- Sidecar persistence (.material.graph.json next to .material.json) ---

    private string? GraphSidecarPath =>
        Asset == null ? null : Path.ChangeExtension(Asset.FullPath, ".material.graph.json");

    private void ReloadGraphFromSidecar()
    {
        EnsureGraphSession();
        var sidecar = GraphSidecarPath;
        if (GraphSession == null || sidecar == null || !File.Exists(sidecar)) return;

        // Tear down existing state, load fresh graph from the sidecar JSON.
        MaterialGraphNative.Destroy(GraphSession.GraphId);
        var fresh = MaterialGraphSession.LoadFromFile(sidecar);
        GraphSession = fresh;
        MaterialGraphSessionLocator.Current = fresh;
        RebuildFromSession();
        PushGraphToPreview();
        ClearDirty();
    }

    /// <summary>
    /// Transform raw sidecar JSON into the transient copy handed to the
    /// native preview renderer. The stored graph keeps portable texture
    /// references ("Content/Assets/X.texture") that the renderer cannot
    /// resolve on its own; every preview push must expand them to absolute
    /// paths via <see cref="MaterialGraphSessionLocator.PreparePreviewJson"/> —
    /// pushing the raw serialization makes the preview drop all texture
    /// bindings until the next edit re-pushes a prepared copy.
    /// </summary>
    internal static string PreviewPushJson(string rawSidecarJson) =>
        MaterialGraphSessionLocator.PreparePreviewJson(rawSidecarJson);

    private void SaveGraphToSidecar()
    {
        if (GraphSession == null) return;
        var sidecar = GraphSidecarPath;
        if (sidecar == null) return;

        // Push latest serialized JSON so the file on disk matches in-memory state,
        // even if the user only edited pin params (not the topology).
        GraphSession.Execute();
        var json = GraphSession.SerializeJson();
        if (json == null) return;

        var dir = Path.GetDirectoryName(sidecar);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(sidecar, json);

        // Mirror the just-saved graph into the live preview so the right-hand
        // pane reflects the same state the sidecar captures. The disk copy
        // stays portable; only the preview copy gets paths expanded.
        if (!string.IsNullOrEmpty(json)) Preview.PushGraph(PreviewPushJson(json));
    }
}

/// <summary>
/// ConnectionViewModelBase requires a NodifyEditorViewModelBase. The
/// material editor doesn't bind through a full editor-VM layer (it uses
/// inline ItemsSource bindings on NodifyEditor), so this placeholder
/// satisfies the ctor contract. The Connections capture the real
/// MaterialEditorViewModel separately via the second ctor arg so they
/// can call back for disconnect UX. We still need an instance of this
/// type to satisfy the base class's signature.
/// </summary>
internal sealed class EditorViewModelPlaceholder : NodifyM.Avalonia.ViewModelBase.NodifyEditorViewModelBase
{
    public static readonly EditorViewModelPlaceholder Instance = new();
}

/// Minimal <see cref="ICommand"/> for
/// <see cref="MaterialEditorViewModel.ConnectionCompletedCommand"/>.
/// Unlike <c>RelayCommand&lt;T&gt;</c> (which throws
/// <see cref="System.ArgumentException"/> from <c>CanExecute</c> when the
/// argument isn't of type <c>T</c>), this implementation accepts any
/// <c>object</c> parameter and forwards it to the wrapped delegate. The
/// delegate is responsible for shape-checking the payload. NodifyM
/// 12.0.0 invokes connection-completed with a
/// generic <c>ValueTuple</c>; a strongly-typed
/// <c>RelayCommand</c> rejects that runtime, so the editor crashed
/// whenever the user dragged a connection between two pins.
/// </summary>
internal sealed class ConnectionCompletedRelayCommand : System.Windows.Input.ICommand
{
    private readonly System.Action<object?> _execute;

    public ConnectionCompletedRelayCommand(System.Action<object?> execute)
    {
        _execute = execute ?? throw new System.ArgumentNullException(nameof(execute));
    }

    public bool CanExecute(object? parameter) => true;

    public void Execute(object? parameter) => _execute(parameter);

    // Avalonia does not provide a CommandManager.RequerySuggested
    // equivalent; the editor surface always calls CanExecute just
    // before Execute, and we always return true, so wiring this
    // event is unnecessary.
    public event System.EventHandler? CanExecuteChanged
    {
        add { }
        remove { }
    }
}
