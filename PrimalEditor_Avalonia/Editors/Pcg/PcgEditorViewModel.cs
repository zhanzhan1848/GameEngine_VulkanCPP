using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text.Json;
using Avalonia;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Pcg;

/// <summary>
/// A6 PCG 图编辑器。引擎侧 ABI（EngineDLL/PCGAPI.cpp，单全局图实例）承载
/// 图模型；本 VM 维护画布镜像并与引擎逐操作同步。加载 = 文件 JSON 引擎
/// Deserialize + C# 解析同 JSON 重建画布；保存 = 画布状态已在引擎，
/// PCGSerializeGraph 写盘。执行 = PCGExecute + 错误/输出点数回显。
/// 留痕：PCGSerializer 的 JSON 无节点坐标字段，画布布局不做持久化。
/// </summary>
public sealed class PcgEditorViewModel : AssetEditorViewModel
{
    private string _json = string.Empty;
    private bool _loaded;
    private int _newNodeGrid;

    public ObservableCollection<PcgNodeViewModel> Nodes { get; } = new();
    public ObservableCollection<PcgConnectionViewModel> Connections { get; } = new();

    private PcgNodeViewModel? _selectedNode;
    public PcgNodeViewModel? SelectedNode
    {
        get => _selectedNode;
        set
        {
            if (SetProperty(ref _selectedNode, value))
                OnPropertyChanged(nameof(SelectedNodeParameters));
        }
    }

    public System.Collections.IEnumerable SelectedNodeParameters
        => (System.Collections.IEnumerable?)SelectedNode?.Parameters ?? Array.Empty<PcgParamViewModel>();

    private string _statusLine = "Ready";
    public string StatusLine
    {
        get => _statusLine;
        private set => SetProperty(ref _statusLine, value);
    }

    public IReadOnlyList<string> PaletteTypes { get; private set; } = Array.Empty<string>();

    public Common.RelayCommand DeleteSelectedCommand { get; }
    public Common.RelayCommand ExecuteCommand { get; }
    public Common.RelayCommand ApplyToSceneCommand { get; }

    /// <summary>WorldEditorViewModel 注入（编辑器单实例语义，静态中转）：
    /// 把输出点批量落成场景实体，返回创建数（-1 = 未接线）。</summary>
    public static Func<PcgApplyPayload, int>? ApplyToSceneCallback;
    private string GraphName =>
        System.IO.Path.GetFileNameWithoutExtension(Asset?.FileName ?? "Pcg");
    public System.Windows.Input.ICommand ConnectionCompletedCommand { get; }

    public PcgEditorViewModel()
    {
        DeleteSelectedCommand = new Common.RelayCommand(DeleteSelected);
        ExecuteCommand = new Common.RelayCommand(Execute);
        ApplyToSceneCommand = new Common.RelayCommand(ApplyToScene);
        // NodifyM 12.0.0 以 (SourceConnector, TargetConnector) ValueTuple 调用
        // ConnectionCompletedCommand——必须走运行时形状识别（镜像材质编辑器的
        // ConnectionCompletedRelayCommand），强类型 RelayCommand 会抛
        // TargetInvocationException。
        ConnectionCompletedCommand = new ConnectionCompletedRelayCommand(
            p => { if (TryGetConnectionPins(p, out var src, out var dst)) OnConnectionCompleted(src, dst); });
        PaletteTypes = Enumerable.Range(0, (int)NativeEngineBridge.Pcg.GetRegisteredNodeTypeCount())
            .Select(i => NativeEngineBridge.Pcg.GetRegisteredNodeTypeName((uint)i) ?? "?")
            .Where(n => n != "?")
            .ToList();
    }

    // ---- 资产生命周期 ------------------------------------------------------

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        LoadFromAsset();
    }

    private bool _isDirty;
    public override bool IsDirty => _isDirty;
    public override void MarkDirty() => _isDirty = true;
    public override void ClearDirty() => _isDirty = false;

    public override void Save()
    {
        if (Asset == null || !_loaded) return;
        var json = NativeEngineBridge.Pcg.SerializeGraph();
        if (json == null)
        {
            StatusLine = "Save failed: engine graph unavailable";
            return;
        }
        // A6 增量：画布坐标 merge 进 JSON（"pos":[x,y]，nodes 序与画布序
        // 一致）。引擎解析器按名取字段、天然忽略未知字段——旧版本引擎读
        // 新文件不受影响。
        json = MergePositions(json, Nodes);
        File.WriteAllText(Asset.FullPath, json + Environment.NewLine);
        ClearDirty();
        StatusLine = $"Saved {Asset.FileName}";
        Logger.Info($"[PCG] saved {Asset.FullPath}");
    }

    // ---- 坐标持久化（纯函数，单测覆盖） ------------------------------------

    /// <summary>把画布坐标按 nodes 数组序 merge 进引擎 JSON（"pos":[x,y]）。
    /// 引擎 JSON 无 nodes 数组或为空时原样返回。</summary>
    public static string MergePositions(string engineJson, System.Collections.IList locations)
    {
        try
        {
            var root = System.Text.Json.Nodes.JsonNode.Parse(engineJson);
            if (root?["nodes"] is not System.Text.Json.Nodes.JsonArray nodes) return engineJson;
            var count = Math.Min(nodes.Count, locations.Count);
            for (int i = 0; i < count; ++i)
            {
                if (locations[i] is not PcgNodeViewModel vm) continue;
                var arr = new System.Text.Json.Nodes.JsonArray
                {
                    (System.Text.Json.Nodes.JsonNode)Math.Round(vm.Location.X, 1),
                    (System.Text.Json.Nodes.JsonNode)Math.Round(vm.Location.Y, 1),
                };
                nodes[i]!["pos"] = arr;
            }
            return root.ToJsonString();
        }
        catch (Exception)
        {
            return engineJson; // merge 失败不阻塞保存——坐标是次要数据
        }
    }

    /// <summary>读节点对象的 "pos":[x,y]；缺失/畸形返回 null（调用方回退网格摆位）。</summary>
    public static Point? TryReadPos(JsonElement node)
    {
        if (!node.TryGetProperty("pos", out var pos) || pos.ValueKind != JsonValueKind.Array)
            return null;
        var arr = pos.EnumerateArray().ToArray();
        if (arr.Length < 2 || arr.Any(e => e.ValueKind != JsonValueKind.Number))
            return null;
        return new Point(arr[0].GetSingle(), arr[1].GetSingle());
    }

    public override void Discard()
    {
        if (Asset != null) LoadFromAsset();
        ClearDirty();
    }

    // ---- 加载 --------------------------------------------------------------

    private void LoadFromAsset()
    {
        Nodes.Clear();
        Connections.Clear();
        SelectedNode = null;
        _loaded = false;
        if (Asset == null || !File.Exists(Asset.FullPath))
        {
            StatusLine = "Asset file missing";
            return;
        }

        try
        {
            _json = File.ReadAllText(Asset.FullPath);
        }
        catch (Exception ex)
        {
            StatusLine = $"Read failed: {ex.Message}";
            return;
        }

        NativeEngineBridge.Pcg.CreateGraph();
        if (!NativeEngineBridge.Pcg.DeserializeGraph(_json))
        {
            StatusLine = "Engine rejected the graph JSON";
            return;
        }

        // C# 侧解析同一 JSON 重建画布镜像（params 初值也来自这里——引擎侧
        // 无 GetParam 值导出）。
        try
        {
            using var doc = JsonDocument.Parse(_json);
            if (doc.RootElement.TryGetProperty("mesh_slots", out var slots))
                StatusLine = $"{slots.GetArrayLength()} mesh slot(s); ";

            if (doc.RootElement.TryGetProperty("nodes", out var nodes))
            {
                uint index = 0;
                foreach (var n in nodes.EnumerateArray())
                {
                    var type = n.GetProperty("type").GetString() ?? "?";
                    var vm = CreateNodeVm(index, type);
                    if (n.TryGetProperty("params", out var ps))
                        FillParams(vm, ps);
                    // 编辑器坐标持久化（A6 增量）：有 pos 用 pos，否则网格摆位。
                    vm.Location = TryReadPos(n)
                        ?? new Point(60 + (index % 4) * 220, 50 + (index / 4) * 160);
                    Nodes.Add(vm);
                    ++index;
                }
            }

            if (doc.RootElement.TryGetProperty("connections", out var conns))
            {
                foreach (var c in conns.EnumerateArray())
                {
                    AddConnectionVm(
                        c.GetProperty("from_node").GetUInt32(),
                        c.GetProperty("from_pin").GetUInt32(),
                        c.GetProperty("to_node").GetUInt32(),
                        c.GetProperty("to_pin").GetUInt32());
                }
            }

            _loaded = true;
            _json = string.Empty; // 释放；保存走引擎 SerializeGraph
            StatusLine += $"{Nodes.Count} node(s), {Connections.Count} connection(s)";
        }
        catch (Exception ex)
        {
            StatusLine = $"Parse failed: {ex.Message}";
        }
    }

    private PcgNodeViewModel CreateNodeVm(uint engineId, string typeName)
    {
        var vm = new PcgNodeViewModel(engineId, typeName);
        // Pins: PCGGetNodePinCount 没有 C# 绑定——直接循环 GetPinInfo 直到 null。
        for (uint i = 0; ; ++i)
        {
            var pin = NativeEngineBridge.Pcg.GetPinInfo(engineId, i);
            if (pin == null) break;
            var pinVm = new PcgPinViewModel(engineId, pin.Index, pin.Type, pin.IsInput, pin.Name);
            if (pin.IsInput) vm.Input.Add(pinVm);
            else vm.Output.Add(pinVm);
        }
        for (uint i = 0; ; ++i)
        {
            var p = NativeEngineBridge.Pcg.GetParamInfo(engineId, i);
            if (p == null) break;
            vm.Parameters.Add(new PcgParamViewModel(engineId, p.Name, p.Group, p.Type,
                p.Min, p.Max, p.Step, p.EnumNames, 0f, 0f, 0f, MarkDirty));
        }
        return vm;
    }

    private void FillParams(PcgNodeViewModel vm, JsonElement paramsElement)
    {
        foreach (var p in vm.Parameters)
        {
            if (!paramsElement.TryGetProperty(p.Name, out var v)) continue;
            switch (v.ValueKind)
            {
                case JsonValueKind.Array when p.IsVec3:
                {
                    var arr = v.EnumerateArray().Take(3).Select(e => e.GetSingle()).ToArray();
                    if (arr.Length == 3)
                    {
                        p.X = arr[0]; p.Y = arr[1]; p.Z = arr[2];
                    }
                    break;
                }
                case JsonValueKind.Array when p.IsFloatArray:
                    // FloatArray (MeshAssign weights) v1 只读展示
                    p.X = (float)v.GetArrayLength();
                    break;
                case JsonValueKind.Number:
                    p.X = v.GetSingle();
                    break;
                case JsonValueKind.True: p.X = 1f; break;
                case JsonValueKind.False: p.X = 0f; break;
                case JsonValueKind.String when p.IsEnum:
                    var label = v.GetString();
                    var idx = Array.IndexOf(p.EnumOptions, label ?? "");
                    p.X = idx >= 0 ? idx : 0f;
                    break;
            }
        }
    }

    // ---- 画布编辑 ----------------------------------------------------------

    public void AddNode(string? typeName)
    {
        if (!_loaded || string.IsNullOrEmpty(typeName)) return;
        var id = NativeEngineBridge.Pcg.AddNode(typeName);
        if (id == 0xFFFFFFFF)
        {
            StatusLine = $"AddNode({typeName}) failed";
            return;
        }
        var vm = CreateNodeVm(id, typeName);
        vm.Location = new Point(80 + (_newNodeGrid % 4) * 220, 60 + (_newNodeGrid / 4) * 160);
        ++_newNodeGrid;
        Nodes.Add(vm);
        SelectedNode = vm;
        MarkDirty();
        StatusLine = $"Added {typeName} (id {id})";
    }

    public void DeleteNode(PcgNodeViewModel node)
    {
        if (!_loaded || node == null) return;
        NativeEngineBridge.Pcg.RemoveNode(node.EngineId);

        // 引擎语义：被删节点之后的 id 前移。画布镜像同步。
        foreach (var n in Nodes.Where(n => n.EngineId > node.EngineId).ToList())
            n.EngineId -= 1;

        // 连接端点 id 同步；涉及被删节点的连接移除。
        for (int i = Connections.Count - 1; i >= 0; --i)
        {
            var c = Connections[i];
            bool touchesRemoved = c.FromNodeId == node.EngineId || c.ToNodeId == node.EngineId;
            if (touchesRemoved)
            {
                Connections.RemoveAt(i);
                continue;
            }
            if (c.FromNodeId > node.EngineId) c.FromNodeId -= 1;
            if (c.ToNodeId > node.EngineId) c.ToNodeId -= 1;
        }

        Nodes.Remove(node);
        if (SelectedNode == node) SelectedNode = null;
        MarkDirty();
        StatusLine = $"Deleted {node.TypeName}";
    }

    public void DeleteSelected()
    {
        if (SelectedNode != null) DeleteNode(SelectedNode);
    }

    internal static bool TryGetConnectionPins(
        object? parameter, out PcgPinViewModel source, out PcgPinViewModel target)
    {
        object? sourceObject = null;
        object? targetObject = null;
        if (parameter is NodifyM.Avalonia.Events.PendingConnectionEventArgs pending)
        {
            sourceObject = pending.SourceConnector;
            targetObject = pending.TargetConnector;
        }
        else if (parameter is System.Runtime.CompilerServices.ITuple tuple && tuple.Length >= 2)
        {
            sourceObject = tuple[0];
            targetObject = tuple[1];
        }
        source = sourceObject as PcgPinViewModel ?? null!;
        target = targetObject as PcgPinViewModel ?? null!;
        return source != null && target != null;
    }

    private sealed class ConnectionCompletedRelayCommand : System.Windows.Input.ICommand
    {
        private readonly System.Action<object?> _execute;
        public ConnectionCompletedRelayCommand(System.Action<object?> execute) =>
            _execute = execute ?? throw new ArgumentNullException(nameof(execute));
        public bool CanExecute(object? parameter) => true;
        public void Execute(object? parameter) => _execute(parameter);
        public event EventHandler? CanExecuteChanged { add { } remove { } }
    }

    /// <summary>NodifyM ConnectionCompletedCommand：新连接两端 pin VM。</summary>
    public void OnConnectionCompleted(object? source, object? target)
    {
        if (!_loaded || source is not PcgPinViewModel from || target is not PcgPinViewModel to) return;
        if (from.IsInput == to.IsInput) return; // 同向 pin 不连
        if (from.IsInput) (from, to) = (to, from); // 统一为 out→in
        NativeEngineBridge.Pcg.Connect(from.NodeId, from.PinIndex, to.NodeId, to.PinIndex);
        AddConnectionVm(from.NodeId, from.PinIndex, to.NodeId, to.PinIndex);
        MarkDirty();
    }

    public void DeleteConnection(PcgConnectionViewModel c)
    {
        if (!_loaded) return;
        NativeEngineBridge.Pcg.Disconnect(c.FromNodeId, c.FromPin, c.ToNodeId, c.ToPin);
        Connections.Remove(c);
        MarkDirty();
    }

    private void AddConnectionVm(uint fromNode, uint fromPin, uint toNode, uint toPin)
    {
        var from = FindPin(Nodes.FirstOrDefault(n => n.EngineId == fromNode), fromPin, isInput: false);
        var to = FindPin(Nodes.FirstOrDefault(n => n.EngineId == toNode), toPin, isInput: true);
        if (from == null || to == null)
        {
            StatusLine = $"Connection {fromNode}.{fromPin}→{toNode}.{toPin}: pins not on canvas";
            return;
        }
        Connections.Add(new PcgConnectionViewModel(from, to, fromNode, fromPin, toNode, toPin));
    }

    private static PcgPinViewModel? FindPin(PcgNodeViewModel? node, uint pinIndex, bool isInput)
    {
        if (node == null) return null;
        var pins = isInput ? node.Input : node.Output;
        return pins.FirstOrDefault(p => p.PinIndex == pinIndex);
    }

    // ---- 执行 --------------------------------------------------------------

    public void Execute()
    {
        if (!_loaded) return;
        NativeEngineBridge.Pcg.Execute();
        foreach (var n in Nodes) n.RefreshOutputPoints();

        var errors = NativeEngineBridge.Pcg.GetErrorCount();
        if (errors > 0)
        {
            var first = NativeEngineBridge.Pcg.GetErrorMessage(0);
            StatusLine = $"Execute: {errors} error(s). First: {first}";
        }
        else
        {
            StatusLine = $"Execute OK — {Nodes.Sum(n => n.OutputPoints)} point(s) across {Nodes.Count} node(s)";
        }
    }

    // ---- 应用到场景 ---------------------------------------------------------

    /// <summary>目标节点：选中节点优先，否则最后一个有输出点的节点。</summary>
    private PcgNodeViewModel? ResolveApplyNode()
    {
        if (SelectedNode != null && SelectedNode.OutputPoints > 0) return SelectedNode;
        return Nodes.LastOrDefault(n => n.OutputPoints > 0);
    }

    public void ApplyToScene()
    {
        if (ApplyToSceneCallback == null)
        {
            StatusLine = "Apply: no scene callback wired";
            return;
        }
        var node = ResolveApplyNode();
        if (node == null)
        {
            StatusLine = "Apply: run Execute first (no node produced points)";
            return;
        }

        var count = node.OutputPoints;
        var payload = new PcgApplyPayload
        {
            GraphName = GraphName,
            Positions = NativeEngineBridge.Pcg.GetOutputPositions(node.EngineId, count),
            Transforms = NativeEngineBridge.Pcg.GetOutputTransforms(node.EngineId, count),
            MeshSlots = NativeEngineBridge.Pcg.GetOutputMeshSlots(node.EngineId, count),
        };
        for (uint s = 0; s < NativeEngineBridge.Pcg.GetMeshSlotCount(); ++s)
        {
            if (NativeEngineBridge.Pcg.GetMeshSlot(s) is { } slot)
                payload.MeshSlotPaths.Add(slot);
        }

        var created = ApplyToSceneCallback(payload);
        StatusLine = created >= 0
            ? $"Applied {created} point(s) from {node.TypeName} to the scene"
            : "Apply: scene unavailable";
    }
}
