using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Entities;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.MaterialGraph;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// 材质资产域 verbs（CLI 规划 §5.1 editor 通道）：批创建、节点图连线、赋实体槽位。
/// 三条铁律：
///   1. 资产落盘与内容浏览器同路径 —— material.create 走 MaterialSerializer.WriteNew +
///      AssetRegistry.Register（.meta 为 GUID 权威源）；图 sidecar 命名沿用编辑器约定
///      （Path.ChangeExtension(fullPath, ".material.graph.json")，即 foo.material.json →
///      foo.material.material.graph.json），SceneRuntimeSynchronizer.EnsureMaterialGraph
///      按同一约定解析；
///   2. 图编辑走 MaterialGraphSession（EngineDLL C ABI 的托管封装，与材质编辑器
///      MaterialEditorViewModel 完全同一套 API），不发明第二序列化通道；
///   3. entity.set-material 只动文档模型（MeshRendererComponent.MaterialGuids，记入
///      undo 栈）再 SceneSynchronizer.RefreshEntityMaterials 热推 —— 与 Inspector
///      PushMaterialsToNativeCallback 同路径。
/// 所有 handler 由 CliHost 编组到 UI 线程执行。
/// </summary>
public sealed class MaterialCommands
{
    /// <summary>MaterialParamType 的稳定 JSON 名（与引擎枚举顺序一致）。</summary>
    private static readonly string[] MaterialParamTypeNames =
    {
        "Bool", "Float", "Float2", "Float3", "Float4", "Int", "UInt", "Enum", "Texture", "Curve",
    };

    // === 共享前置解析 ===

    private static WorldEditorViewModel RequireWorldEditor() =>
        EditorServices.WorldEditor
        ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");

    private static GameEntity RequireEntity(Guid entityGuid)
    {
        var scene = EditorServices.ActiveScene
            ?? throw CliError.NotReady("当前没有激活场景", "先在编辑器中打开项目");
        var entity = FindEntity(scene.Entities, entityGuid)
            ?? throw CliError.NotFound(
                $"实体 {entityGuid.ToString().ToLowerInvariant()}",
                "先用 scene.tree 查询有效 guid（实体可能已被删除）");
        return entity;
    }

    private static GameEntity? FindEntity(IEnumerable<GameEntity> entities, Guid guid)
    {
        foreach (var entity in entities)
        {
            if (entity.EntityGuid.Value == guid) return entity;
            var inChildren = FindEntity(entity.Children, guid);
            if (inChildren != null) return inChildren;
        }
        return null;
    }

    /// <summary>path / guid 二选一解析材质资产。path 接受绝对路径或项目相对路径。</summary>
    private static (AssetInfo Asset, WorldEditorViewModel Vm) RequireMaterial(string? path, string? guid)
    {
        var vm = RequireWorldEditor();
        if ((path == null) == (guid == null))
            throw CliError.InvalidParams("path 与 guid 必须二选一提供",
                @"示例: {""path"":""Content/Materials/Brass.material.json""} 或 {""guid"":""…""}");
        AssetInfo? info;
        if (guid != null)
        {
            if (!Guid.TryParse(guid, out var g) || g == Guid.Empty)
                throw CliError.InvalidParams("guid 不是合法 Guid", @"示例: ""0c9e6c9a-2e5f-4a5b-8b1e-3f2a4d5c6b7a""");
            info = AssetRegistry.GetByGuid(g)
                ?? throw CliError.NotFound($"材质 {guid}", "用 asset.list type=Material 查询有效 guid");
        }
        else
        {
            var full = ResolveProjectPath(vm, path!);
            info = AssetRegistry.Get(full)
                ?? throw CliError.NotFound($"材质 '{path}'",
                    "路径基于项目根；新建用 material.create，已有资产用 asset.list 查询");
        }
        if (info.Type != AssetType.Material)
            throw CliError.InvalidParams($"'{info.FullPath}' 不是材质资产（type={info.Type}）",
                "材质文件以 .material.json 结尾");
        return (info, vm);
    }

    private static string ResolveProjectPath(WorldEditorViewModel vm, string path)
    {
        var root = vm.Project?.Path
            ?? throw CliError.NotReady("没有已打开的项目", "先在编辑器中打开项目");
        var full = Path.IsPathRooted(path)
            ? Path.GetFullPath(path)
            : Path.GetFullPath(Path.Combine(root, path));
        if (!full.StartsWith(Path.GetFullPath(root), StringComparison.Ordinal))
            throw CliError.InvalidParams($"路径 '{path}' 越出项目根", "使用项目内相对路径，例: Content/Materials/x.material.json");
        return full;
    }

    /// <summary>sidecar 约定：Path.ChangeExtension(fullPath, ".material.graph.json")。
    /// 与 SceneRuntimeSynchronizer.EnsureMaterialGraph / 材质编辑器 GraphSidecarPath
    /// 完全一致（foo.material.json → foo.material.material.graph.json）。</summary>
    internal static string SidecarPathFor(string materialFullPath) =>
        Path.ChangeExtension(materialFullPath, ".material.graph.json");

    /// <summary>
    /// 打开 sidecar → 变更 → Execute 校验 → 序列化落盘（与材质编辑器
    /// SaveGraphToSidecar 相同的 File.WriteAllText 收尾）。返回执行错误列表
    /// （图仍会保存——与编辑器一致，错误随响应返回供调用方修正）。
    /// </summary>
    private static IReadOnlyList<(uint NodeId, string Message)> EditSidecar(string sidecarPath, Action<MaterialGraphSession> mutate)
    {
        if (!NativeEngineBridge.IsLoaded)
            throw CliError.NotReady("引擎未加载，材质图编辑不可用", "用 editor.ping 查看 engineLoaded");
        if (!File.Exists(sidecarPath))
            throw CliError.NotFound($"材质图 sidecar '{sidecarPath}'",
                "material.create graph=true 先创建带图材质；已有材质用材质编辑器添加图");
        using var session = MaterialGraphSession.LoadFromFile(sidecarPath)
            ?? throw new CliError(CliErrorCodes.EngineError, $"材质图 sidecar 解析失败: {sidecarPath}", "检查 JSON 是否被手工改坏");
        mutate(session);
        session.Execute();
        var errors = session.GetErrors();
        var json = session.SerializeJson()
            ?? throw new CliError(CliErrorCodes.EngineError, "材质图序列化失败", "重试；持续失败检查引擎日志");
        File.WriteAllText(sidecarPath, json);
        return errors;
    }

    private static System.Text.Json.JsonElement ParseGraphJson(string json)
    {
        var doc = System.Text.Json.JsonDocument.Parse(json);
        return doc.RootElement.Clone();
    }

    // === 创建 ===

    [CliCommand("material.create",
        Description = "创建材质资产（与内容浏览器“新建材质”同路径）：写 <folder>/<name>.material.json 并注册 AssetRegistry" +
                      "（.meta 落 GUID）。graph=true 同时创建最小合法图 sidecar（单个 MaterialOutput 节点，" +
                      "1-based 节点 id=1），后续用 material.graph.* 连线。folder 为项目相对目录，默认项目 Content 根。" +
                      "name 只允许字母/数字/下划线/连字符（防止路径注入）；目标文件已存在返回 ALREADY_EXISTS。",
        Example = """{"name":"Brass","folder":"Content/Materials","graph":true}""")]
    public object Create(string name, string? folder = null, bool graph = true)
    {
        var vm = RequireWorldEditor();
        var contentRoot = vm.Project?.ContentPath
            ?? throw CliError.NotReady("没有已打开的项目", "先在编辑器中打开项目");
        if (string.IsNullOrWhiteSpace(name) || name.Trim().Any(c => !char.IsLetterOrDigit(c) && c != '_' && c != '-'))
            throw CliError.InvalidParams("name 必须非空且只含字母/数字/下划线/连字符", @"示例: ""Brass""");
        name = name.Trim();

        var parent = contentRoot;
        if (!string.IsNullOrWhiteSpace(folder))
        {
            parent = ResolveProjectPath(vm, folder);
            if (!Directory.Exists(parent))
                throw CliError.NotFound($"目录 '{folder}'", "folder 必须是项目内已存在的目录（相对项目根）");
        }

        var materialPath = Path.Combine(parent, name + ".material.json");
        if (File.Exists(materialPath))
            throw new CliError(CliErrorCodes.InvalidParams, $"材质已存在: {materialPath}",
                "换名或直接用 material.graph.* 编辑现有材质");

        MaterialSerializer.WriteNew(materialPath, name);
        var info = AssetRegistry.Register(materialPath)
            ?? throw new CliError(CliErrorCodes.EngineError, "材质注册失败（AssetRegistry 未初始化？）",
                "确认编辑器已打开项目（asset.list 可查询即已初始化）");

        string? sidecarRelative = null;
        uint outputNodeId = 0;
        if (graph)
        {
            if (!NativeEngineBridge.IsLoaded)
                throw CliError.NotReady("引擎未加载，无法创建图 sidecar（材质文件已创建）",
                    "engineLoaded=true 后重试 material.graph.* 或改用 graph=false");
            var sidecar = SidecarPathFor(materialPath);
            using var session = MaterialGraphSession.Create();
            outputNodeId = session.AddNode("MaterialOutput");
            session.SetNodePosition(outputNodeId, 240f, 0f);
            session.Execute();
            var json = session.SerializeJson();
            if (outputNodeId == 0 || json == null)
                throw new CliError(CliErrorCodes.EngineError, "初始 MaterialOutput 节点创建失败",
                    "检查引擎日志；材质文件已落盘，可改用材质编辑器补图");
            Directory.CreateDirectory(Path.GetDirectoryName(sidecar)!);
            File.WriteAllText(sidecar, json);
            AssetRegistry.Register(sidecar);
            sidecarRelative = Path.GetRelativePath(vm.Project!.Path, sidecar).Replace('\\', '/');
        }

        Logger.Info($"[cli] material.create '{name}' → {materialPath} (graph={graph})");
        return new
        {
            created = true,
            name,
            path = Path.GetRelativePath(vm.Project!.Path, materialPath).Replace('\\', '/'),
            guid = info?.Guid.ToString("D").ToLowerInvariant(),
            graph,
            sidecar = sidecarRelative,
            outputNodeId,
            hint = "连线: material.graph.add-node → material.graph.connect（into node=outputNodeId pin=0）→ entity.set-material 赋给实体",
        };
    }

    // === 节点图编辑 ===

    [CliCommand("material.graph.add-node",
        Description = "向材质图 sidecar 追加一个节点并保存。type 为注册节点类型名" +
                      "（material.graph.node-types 可枚举，如 ConstantFloat3/Add/Multiply/SampleTexture/MaterialOutput）。" +
                      "x/y 为编辑器画布坐标（可选，缺省按现有节点数网格排布）。" +
                      "返回 1-based nodeId（0 表示失败；后续 connect/set-param 用它寻址）。" +
                      "材质无 sidecar 返回 NOT_FOUND；type 未注册返回 INVALID_PARAMS。",
        Example = """{"path":"Content/Materials/Brass.material.json","type":"ConstantFloat3"}""")]
    public object GraphAddNode(string? path = null, string? guid = null, string type = "ConstantFloat3",
        double? x = null, double? y = null)
    {
        var (asset, _) = RequireMaterial(path, guid);
        var sidecar = SidecarPathFor(asset.FullPath);
        uint nodeId = 0;
        uint nodeCount = 0;
        var errors = EditSidecar(sidecar, session =>
        {
            nodeId = session.AddNode(type);
            nodeCount = session.NodeCount;
            if (nodeId != 0)
            {
                var px = (float)(x ?? (-280 + 70 * (nodeCount % 6)));
                var py = (float)(y ?? 40f * nodeCount);
                session.SetNodePosition(nodeId, px, py);
            }
        });
        if (nodeId == 0)
            throw CliError.InvalidParams($"未知节点类型 '{type}'",
                "material.graph.node-types 枚举有效类型");
        return new { added = true, nodeId, type, nodeCount, errors = errors.Select(e => e.Message).ToArray() };
    }

    [CliCommand("material.graph.set-param",
        Description = "设置材质图中某节点的参数并保存。node 为 1-based nodeId（material.graph.get 查询）。" +
                      "param 为参数名（如 ConstantFloat3.value / ConstantTexture.asset_path / SampleTexture.srgb）。" +
                      "floats 与 text 二选一：floats 长度 1/2/3/4 分别落 Float/Float2/Float3/Float4；" +
                      "text 落 String（如贴图相对路径）。参数名或类型不匹配时该参数静默失败——" +
                      "响应里 paramApplied=false 即未生效，用 material.graph.node-types 查描述符。",
        Example = """{"path":"Content/Materials/Brass.material.json","node":2,"param":"value","floats":[0.8,0.6,0.2]}""")]
    public object GraphSetParam(string? path = null, string? guid = null, int node = 0, string param = "",
        float[]? floats = null, string? text = null)
    {
        var (asset, _) = RequireMaterial(path, guid);
        if (node < 1) throw CliError.InvalidParams("node 为 1-based nodeId，必须 ≥ 1", @"示例: 2");
        if (string.IsNullOrWhiteSpace(param))
            throw CliError.InvalidParams("param 不能为空", @"示例: ""value""");
        if (floats == null && text == null)
            throw CliError.InvalidParams("floats 与 text 必须二选一", @"示例: {""floats"":[1,0,0]}");
        if (floats is { Length: < 1 or > 4 })
            throw CliError.InvalidParams("floats 长度必须是 1..4", @"示例: [1,0,0]");

        var sidecar = SidecarPathFor(asset.FullPath);
        var applied = false;
        var errors = EditSidecar(sidecar, session =>
        {
            if (floats != null)
            {
                applied = floats.Length switch
                {
                    1 => session.SetParamFloat((uint)node, param, floats[0]),
                    2 => session.SetParamFloat2((uint)node, param, floats[0], floats[1]),
                    3 => session.SetParamFloat3((uint)node, param, floats[0], floats[1], floats[2]),
                    _ => session.SetParamFloat4((uint)node, param, floats[0], floats[1], floats[2], floats[3]),
                };
            }
            else
            {
                applied = session.SetParamString((uint)node, param, text!);
            }
        });
        return new { node, param, paramApplied = applied, errors = errors.Select(e => e.Message).ToArray() };
    }

    [CliCommand("material.graph.connect",
        Description = "在材质图两点间连一条边并保存。fromNode/toNode 为 1-based nodeId，" +
                      "fromPin/toPin 为 0-based 管脚序号（输出脚→输入脚方向）。同一输入脚重连会替换旧边" +
                      "（与编辑器画布一致）。节点/管脚越界或方向非法返回 INVALID_PARAMS。" +
                      "连线后图的编译错误随响应 errors 返回（空数组 = 通过）。",
        Example = """{"path":"Content/Materials/Brass.material.json","fromNode":2,"fromPin":0,"toNode":1,"toPin":0}""")]
    public object GraphConnect(string? path = null, string? guid = null,
        int fromNode = 0, int fromPin = 0, int toNode = 0, int toPin = 0)
    {
        var (asset, _) = RequireMaterial(path, guid);
        if (fromNode < 1 || toNode < 1)
            throw CliError.InvalidParams("fromNode/toNode 为 1-based nodeId，必须 ≥ 1",
                @"示例: {""fromNode"":2,""fromPin"":0,""toNode"":1,""toPin"":0}");
        if (fromPin < 0 || toPin < 0)
            throw CliError.InvalidParams("fromPin/toPin 为 0-based 管脚序号，必须 ≥ 0");
        var sidecar = SidecarPathFor(asset.FullPath);
        var connected = false;
        var errors = EditSidecar(sidecar, session =>
        {
            connected = session.Connect((uint)fromNode, (uint)fromPin, (uint)toNode, (uint)toPin);
        });
        if (!connected)
            throw CliError.InvalidParams(
                $"连线失败: node {fromNode} pin {fromPin} → node {toNode} pin {toPin}",
                "管脚越界或方向错误（只能输出脚→输入脚）；material.graph.get 查看现有图，material.graph.node-types 查看管脚数");
        return new { connected = true, fromNode, fromPin, toNode, toPin, errors = errors.Select(e => e.Message).ToArray() };
    }

    [CliCommand("material.graph.disconnect",
        Description = "移除材质图中一条边并保存。寻址参数与 material.graph.connect 完全一致；" +
                      "边不存在时视为成功（幂等），响应 removed=false。",
        Example = """{"path":"Content/Materials/Brass.material.json","fromNode":2,"fromPin":0,"toNode":1,"toPin":0}""")]
    public object GraphDisconnect(string? path = null, string? guid = null,
        int fromNode = 0, int fromPin = 0, int toNode = 0, int toPin = 0)
    {
        var (asset, _) = RequireMaterial(path, guid);
        if (fromNode < 1 || toNode < 1)
            throw CliError.InvalidParams("fromNode/toNode 为 1-based nodeId，必须 ≥ 1");
        var sidecar = SidecarPathFor(asset.FullPath);
        var removed = false;
        var errors = EditSidecar(sidecar, session =>
        {
            var before = session.ConnectionCount;
            session.Disconnect((uint)fromNode, (uint)fromPin, (uint)toNode, (uint)toPin);
            removed = session.ConnectionCount < before;
        });
        return new { removed, fromNode, fromPin, toNode, toPin, errors = errors.Select(e => e.Message).ToArray() };
    }

    [CliCommand("material.graph.get",
        Description = "读取材质图当前拓扑：nodes（1-based id + 类型 + 画布坐标）、connections" +
                      "（1-based 端点）、executeErrors（编译错误）与原始 JSON（params 明细在内）。" +
                      "材质没有 sidecar 时返回 hasGraph=false（不是错误）。LLM 修改图前先读本命令建立寻址。",
        Example = """{"path":"Content/Materials/Brass.material.json"}""")]
    public object GraphGet(string? path = null, string? guid = null)
    {
        var (asset, _) = RequireMaterial(path, guid);
        var sidecar = SidecarPathFor(asset.FullPath);
        if (!File.Exists(sidecar))
            return new { hasGraph = false, sidecar = (string?)sidecar, hint = "material.create graph=true 或材质编辑器添加图" };
        if (!NativeEngineBridge.IsLoaded)
            throw CliError.NotReady("引擎未加载，无法解析材质图", "用 editor.ping 查看 engineLoaded");
        using var session = MaterialGraphSession.LoadFromFile(sidecar)
            ?? throw new CliError(CliErrorCodes.EngineError, $"材质图 sidecar 解析失败: {sidecar}", "检查 JSON 是否被手工改坏");
        session.Execute();

        var nodes = new List<object>();
        for (uint i = 0; i < session.NodeCount; i++)
        {
            var id = session.GetNodeIdByIndex(i);
            session.TryGetNodePosition(id, out var px, out var py);
            nodes.Add(new { id, type = session.GetNodeTypeName(id), x = px, y = py });
        }
        var conns = session.Connections
            .Select(c => new { fromNode = c.FromNode, fromPin = c.FromPin, toNode = c.ToNode, toPin = c.ToPin })
            .ToList();
        return new
        {
            hasGraph = true,
            sidecar,
            nodes,
            connections = conns,
            executeErrors = session.GetErrors().Select(e => new { node = e.NodeId, e.Message }).ToArray(),
            json = ParseGraphJson(session.SerializeJson() ?? "{}"),
        };
    }

    [CliCommand("material.graph.node-types",
        Description = "枚举引擎注册的全部材质图节点类型（含显示名/分类/是否输出节点），" +
                      "以及每个类型的输入/输出管脚名与参数描述符（名称/类型/取值范围）。" +
                      "批创建连线前先调用本命令确定 type 名、pin 序号与 param 名。" +
                      "引擎未加载返回 NOT_READY。",
        Example = "{}")]
    public object GraphNodeTypes()
    {
        if (!NativeEngineBridge.IsLoaded)
            throw CliError.NotReady("引擎未加载，节点类型注册表不可用", "用 editor.ping 查看 engineLoaded");
        var types = new List<object>();
        var count = MaterialGraphReflectionNative.GetRegisteredTypeCount();
        for (uint i = 0; i < count; i++)
        {
            if (!MaterialGraphReflectionNative.TryGetRegisteredTypeInfo(i, 128, 128, 64,
                    out var typeName, out var displayName, out var category, out var isTerminal))
                continue;

            // 管脚/参数描述符挂在节点实例上：临时会话逐类型实例化后枚举。
            using var probe = MaterialGraphSession.Create();
            var probeId = probe.AddNode(typeName);
            var inputs = new List<string>();
            var outputs = new List<string>();
            var parameters = new List<object>();
            if (probeId != 0)
            {
                for (uint p = 0; p < probe.GetPinCount(probeId); p++)
                {
                    if (probe.TryGetPin(probeId, p, out var pin, out var pinName))
                        (pin.IsInput != 0 ? inputs : outputs).Add(pinName);
                }
                for (uint p = 0; p < MaterialGraphReflectionNative.GetParamCount(probe.GraphId, probeId); p++)
                {
                    if (!MaterialGraphReflectionNative.TryGetParamDescriptor(
                            probe.GraphId, probeId, p, 128, 64, out var pd, out var paramName, out var group))
                        continue;
                    parameters.Add(new
                    {
                        name = paramName,
                        type = pd.Type < (uint)MaterialParamType.Curve
                            ? MaterialParamTypeNames[(int)pd.Type]
                            : ((MaterialParamType)pd.Type).ToString(),
                        min = pd.MinVal,
                        max = pd.MaxVal,
                        step = pd.Step,
                    });
                }
            }
            types.Add(new { type = typeName, displayName, category, isTerminal, inputs, outputs, parameters });
        }
        return new { count = types.Count, types };
    }

    // === 实体材质槽 ===

    [CliCommand("entity.set-material",
        Description = "把材质资产赋给实体 MeshRendererComponent 的材质槽（与 Inspector 编辑同路径：" +
                      "先落文档模型记入 undo，再 SceneSynchronizer.RefreshEntityMaterials 热推引擎——" +
                      "若该材质带图 sidecar 会立即编译并应用）。slot 为 0-based 槽位，缺省 0；" +
                      "slot == 当前槽数 = 追加新槽，超出则 INVALID_PARAMS。材质 guid/path 二选一" +
                      "（path 为 .material.json 的项目相对或绝对路径）。实体没有 MeshRenderer 组件或" +
                      "材质不存在返回 NOT_FOUND。撤销走 editor.undo（与 Inspector 一致）。",
        Example = """{"entityGuid":"…","path":"Content/Materials/Brass.material.json","slot":0}""")]
    public object SetMaterial(Guid entityGuid, string? path = null, string? guid = null, int? slot = null)
    {
        var entity = RequireEntity(entityGuid);
        var (asset, vm) = RequireMaterial(path, guid);
        var renderer = entity.GetComponent<MeshRendererComponent>()
            ?? throw CliError.NotFound($"实体 '{entity.Name}' 没有 MeshRenderer 组件",
                "entity.create kind=Sphere 等带渲染的实体自带该组件；或在 Inspector 手动添加");
        if (slot is { } s && (s < 0 || s > renderer.MaterialGuids.Count))
            throw CliError.InvalidParams($"slot 超界 [0..{renderer.MaterialGuids.Count}]（=槽数表示追加）",
                @"示例: {""slot"":0}");

        var oldGuids = renderer.MaterialGuids.Select(g => g.Value).ToList();
        var newGuids = new List<Guid>(oldGuids);
        var slotIndex = slot ?? 0;
        if (slotIndex == newGuids.Count) newGuids.Add(asset.Guid);
        else newGuids[slotIndex] = asset.Guid;

        vm.UndoRedo.Add(new MaterialSlotsAction(vm, entity, oldGuids, newGuids));
        ApplySlots(renderer, newGuids);
        vm.SceneSynchronizer.RefreshEntityMaterials(entity);
        vm.MarkProjectDirty();

        return new
        {
            entityGuid = entityGuid.ToString("D").ToLowerInvariant(),
            appliedSlot = slotIndex,
            material = new { guid = asset.Guid.ToString("D").ToLowerInvariant(), path = asset.FullPath },            slots = newGuids.Select(g => g.ToString("D").ToLowerInvariant()).ToArray(),
            hint = "editor.screenshot 验证效果；带图材质编译错误看引擎日志",
        };
    }

    private static void ApplySlots(MeshRendererComponent renderer, IReadOnlyList<Guid> guids)
    {
        renderer.MaterialGuids.Clear();
        foreach (var g in guids)
            renderer.MaterialGuids.Add(new AssetGuid(g));
    }

    /// <summary>材质槽快照动作：Undo/Redo 都会触发 RefreshEntityMaterials，
    /// 保证引擎侧编译图/固定材质与文档槽位随时一致。</summary>
    private sealed class MaterialSlotsAction : IUndoRedoAction
    {
        private readonly WorldEditorViewModel _vm;
        private readonly GameEntity _entity;
        private readonly List<Guid> _oldGuids;
        private readonly List<Guid> _newGuids;

        public string Name => $"CLI Set Material ({_entity.Name})";

        public MaterialSlotsAction(WorldEditorViewModel vm, GameEntity entity, List<Guid> oldGuids, List<Guid> newGuids)
        {
            _vm = vm; _entity = entity; _oldGuids = oldGuids; _newGuids = newGuids;
        }

        public void Undo() => Apply(_oldGuids);
        public void Redo() => Apply(_newGuids);

        private void Apply(IReadOnlyList<Guid> guids)
        {
            if (_entity.GetComponent<MeshRendererComponent>() is not { } renderer) return;
            ApplySlots(renderer, guids);
            _vm.SceneSynchronizer.RefreshEntityMaterials(_entity);
        }
    }
}
