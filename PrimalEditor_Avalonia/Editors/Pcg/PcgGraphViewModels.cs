using System;
using System.Collections.ObjectModel;
using System.Globalization;
using Avalonia;
using CommunityToolkit.Mvvm.ComponentModel;
using NodifyM.Avalonia.ViewModelBase;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Pcg;

// A6 PCG 图编辑器 VM 三件套（NodifyM 契约镜像 Editors/Material 同款结构）。

public sealed class PcgParamViewModel : ObservableObject
{
    private readonly Action _markDirty;

    public uint NodeId { get; }
    public string Name { get; }
    public string Group { get; }
    public byte Type { get; }
    public float Min { get; }
    public float Max { get; }
    public float Step { get; }
    public string? EnumNames { get; }

    public PcgParamViewModel(uint nodeId, string name, string group, byte type,
        float min, float max, float step, string? enumNames,
        float x, float y, float z, Action markDirty)
    {
        NodeId = nodeId; Name = name; Group = group; Type = type;
        Min = min; Max = max; Step = step > 0f ? step : 0.01f;
        EnumNames = enumNames; _markDirty = markDirty;
        _x = x; _y = y; _z = z;
    }

    public bool IsBool => Type == 0;
    public bool IsFloat => Type == 1 || Type == 2 || Type == 3; // Float/Int/UInt
    public bool IsEnum => Type == 4;
    public bool IsVec3 => Type == 5;
    public bool IsFloatArray => Type == 6;

    public string[] EnumOptions => (EnumNames ?? string.Empty).Split(',');

    public string DisplayValue => IsBool
        ? (_x != 0f ? "true" : "false")
        : IsVec3 ? $"{_x:0.###}, {_y:0.###}, {_z:0.###}"
        : _x.ToString("0.###", CultureInfo.InvariantCulture);

    private float _x;
    private float _y;
    private float _z;

    public float X
    {
        get => _x;
        set
        {
            if (SetProperty(ref _x, value)) PushScalar();
        }
    }

    public float Y
    {
        get => _y;
        set
        {
            if (SetProperty(ref _y, value)) PushVec3();
        }
    }

    public float Z
    {
        get => _z;
        set
        {
            if (SetProperty(ref _z, value)) PushVec3();
        }
    }

    /// <summary>Bool/Enum 用整数语义写（X 为 0/1 或枚举下标）。</summary>
    public bool BoolValue
    {
        get => _x != 0f;
        set
        {
            var v = value ? 1f : 0f;
            if (SetProperty(ref _x, v))
            {
                NativeEngineBridge.Pcg.SetParamFloat(NodeId, Name, v);
                OnPropertyChanged(nameof(DisplayValue));
                _markDirty();
            }
        }
    }

    public int EnumIndex
    {
        get => (int)_x;
        set
        {
            var v = (float)value;
            if (SetProperty(ref _x, v))
            {
                NativeEngineBridge.Pcg.SetParamFloat(NodeId, Name, v);
                _markDirty();
            }
        }
    }

    private void PushScalar()
    {
        NativeEngineBridge.Pcg.SetParamFloat(NodeId, Name, _x);
        OnPropertyChanged(nameof(DisplayValue));
        _markDirty();
    }

    private void PushVec3()
    {
        NativeEngineBridge.Pcg.SetParamVec3(NodeId, Name, _x, _y, _z);
        OnPropertyChanged(nameof(DisplayValue));
        _markDirty();
    }
}

public sealed class PcgPinViewModel : ConnectorViewModelBase
{
    public uint NodeId { get; }
    public uint PinIndex { get; }
    public byte DataType { get; }
    public bool IsInput { get; }

    public PcgPinViewModel(uint nodeId, uint pinIndex, byte dataType, bool isInput, string title)
    {
        NodeId = nodeId; PinIndex = pinIndex; DataType = dataType; IsInput = isInput;
        Title = title;
    }
}

public sealed class PcgNodeViewModel : NodeViewModelBase
{
    private uint _engineId;
    public uint EngineId
    {
        get => _engineId;
        set => SetProperty(ref _engineId, value);
    }

    public string TypeName { get; }
    public ObservableCollection<PcgPinViewModel> Input { get; } = new();
    public ObservableCollection<PcgPinViewModel> Output { get; } = new();
    public ObservableCollection<PcgParamViewModel> Parameters { get; } = new();

    private Point _location;
    public Point Location
    {
        get => _location;
        set => SetProperty(ref _location, value);
    }

    private uint _outputPoints;
    public uint OutputPoints
    {
        get => _outputPoints;
        private set => SetProperty(ref _outputPoints, value);
    }

    public PcgNodeViewModel(uint engineId, string typeName)
    {
        EngineId = engineId;
        TypeName = typeName;
        Title = typeName;
    }

    public void RefreshOutputPoints()
    {
        OutputPoints = NativeEngineBridge.Pcg.GetOutputPointCount(EngineId);
    }
}

public sealed class PcgConnectionViewModel : ConnectionViewModelBase
{
    // 引擎 RemoveNode 会使后续节点 id 前移——端点 id 必须可同步改写。
    public uint FromNodeId { get; set; }
    public uint FromPin { get; }
    public uint ToNodeId { get; set; }
    public uint ToPin { get; }

    public PcgConnectionViewModel(PcgPinViewModel source, PcgPinViewModel target,
        uint fromNodeId, uint fromPin, uint toNodeId, uint toPin)
        : base(PcgEditorPlaceholder.Instance, source, target)
    {
        Source = source;
        Target = target;
        FromNodeId = fromNodeId; FromPin = fromPin;
        ToNodeId = toNodeId; ToPin = toPin;
    }
}

/// <summary>A6 Apply：PCG 输出点 → 场景实体的数据载荷（由编辑器 VM 从引擎
/// 读回后交给 WorldEditorViewModel 批量建实体）。</summary>
public sealed class PcgApplyPayload
{
    public string GraphName { get; set; } = "PcgPoint";
    public float[] Positions { get; set; } = Array.Empty<float>();    // XYZ × N
    public float[] Transforms { get; set; } = Array.Empty<float>();   // {sx,sy,sz,rotY} × N
    public uint[] MeshSlots { get; set; } = Array.Empty<uint>();      // × N
    public System.Collections.Generic.List<(string Path, string Name)> MeshSlotPaths { get; } = new();
}

/// <summary>ConnectionViewModelBase 需要 NodifyEditorViewModelBase；PCG 编辑器
/// 用内联 ItemsSource 绑定（同材质编辑器），该占位类型仅满足基类签名。</summary>
internal sealed class PcgEditorPlaceholder : NodifyM.Avalonia.ViewModelBase.NodifyEditorViewModelBase
{
    public static readonly PcgEditorPlaceholder Instance = new();
}
