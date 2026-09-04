using System;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Views.Engine3D;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>
/// GI 体积编辑面板数据源（plan §6.2 B1 L1）。体积是管线级配置（非场景实体），
/// 字段以 <see cref="NativeEngineBridge"/> 的 LumenVolumeConfig 不透明缓冲区
/// 偏移读写；Apply 后引擎侧 settings_dirty_ 下帧重建 Lumen passes。
/// probe 球体/拖拽拾取（GIGizmoPick）留待 L1 后续——本版以面板数值编辑 +
/// 视口线框（SelectedBox）为主交互。
/// </summary>
public sealed class GiVolumeViewModel : ViewModelBase
{
    private System.Collections.Generic.List<string> _volumeOptions = new();
    public System.Collections.Generic.IReadOnlyList<string> VolumeOptions => _volumeOptions;

    /// <summary>实例属性（编译绑定无法解析 static 字段）。</summary>
    public System.Collections.Generic.IReadOnlyList<string> ProbeVisOptions { get; } =
        new[] { "Off", "Irradiance Atlas", "Depth Atlas", "Confidence", "Probe Positions" };

    /// <summary>ComboBox 绑定桥（-1 = 无体积）。</summary>
    public int SelectedIndexOption
    {
        get => HasVolumes ? (int)Math.Min(SelectedIndex, (uint)(_volumeOptions.Count - 1)) : -1;
        set { if (value >= 0) SelectedIndex = (uint)value; }
    }

    private bool _hasVolumes;

    public bool HasVolumes
    {
        get => _hasVolumes;
        private set { if (_hasVolumes != value) { _hasVolumes = value; OnPropertyChanged(nameof(HasVolumes)); } }
    }

    private uint _selectedIndexBacking;
    public uint SelectedIndex
    {
        get => _selectedIndexBacking;
        set
        {
            if (_selectedIndexBacking != value)
            {
                _selectedIndexBacking = value;
                OnPropertyChanged(nameof(SelectedIndex));
                LoadSelected();
            }
        }
    }

    private uint _countX = 32;
    public uint CountX { get => _countX; set { if (_countX != value) { _countX = value; OnPropertyChanged(nameof(CountX)); } } }
    private uint _countY = 16;
    public uint CountY { get => _countY; set { if (_countY != value) { _countY = value; OnPropertyChanged(nameof(CountY)); } } }
    private uint _countZ = 32;
    public uint CountZ { get => _countZ; set { if (_countZ != value) { _countZ = value; OnPropertyChanged(nameof(CountZ)); } } }

    private float _spacing = 2f;
    public float Spacing { get => _spacing; set { if (MathF.Abs(_spacing - value) > 1e-6f) { _spacing = value; OnPropertyChanged(nameof(Spacing)); } } }

    private uint _raysPerProbe = 64;
    public uint RaysPerProbe { get => _raysPerProbe; set { if (_raysPerProbe != value) { _raysPerProbe = value; OnPropertyChanged(nameof(RaysPerProbe)); } } }

    private float _maxDistance = 50f;
    public float MaxDistance { get => _maxDistance; set { if (MathF.Abs(_maxDistance - value) > 1e-6f) { _maxDistance = value; OnPropertyChanged(nameof(MaxDistance)); } } }

    private float _originX;
    private float _originY;
    private float _originZ;
    public float OriginX { get => _originX; set { if (MathF.Abs(_originX - value) > 1e-6f) { _originX = value; OnPropertyChanged(nameof(OriginX)); } } }
    public float OriginY { get => _originY; set { if (MathF.Abs(_originY - value) > 1e-6f) { _originY = value; OnPropertyChanged(nameof(OriginY)); } } }
    public float OriginZ { get => _originZ; set { if (MathF.Abs(_originZ - value) > 1e-6f) { _originZ = value; OnPropertyChanged(nameof(OriginZ)); } } }

    private bool _useOriginOverride;
    public bool UseOriginOverride
    {
        get => _useOriginOverride;
        set { if (_useOriginOverride != value) { _useOriginOverride = value; OnPropertyChanged(nameof(UseOriginOverride)); } }
    }

    private int _probeVisIndex;
    public int ProbeVisIndex
    {
        get => _probeVisIndex;
        set
        {
            if (_probeVisIndex != value && value >= 0 && value < ProbeVisOptions.Count)
            {
                _probeVisIndex = value;
                OnPropertyChanged(nameof(ProbeVisIndex));
                NativeEngineBridge.GISetProbeVisMode((uint)value);
            }
        }
    }

    private bool _showProbes;
    /// <summary>在视口绘制 probe 格点（plan §6.2 B1 L1.5）。位置为 CPU 解析
    /// 计算的格点阵（origin + (i+0.5)×spacing），与引擎 Initialize 上传的
    /// probe_positions_ 一致——零 GPU 读回。</summary>
    public bool ShowProbes
    {
        get => _showProbes;
        set { if (_showProbes != value) { _showProbes = value; OnPropertyChanged(nameof(ShowProbes)); } }
    }

    private bool _showProbeSpheres;

    /// <summary>B1 L3: GPU probe 球 overlay（DebugProbeSphereModule，icosphere
    /// 逐 probe + 稳定色相）。开关直推引擎 per-frame bool。</summary>
    public bool ShowProbeSpheres
    {
        get => _showProbeSpheres;
        set
        {
            if (_showProbeSpheres == value) return;
            _showProbeSpheres = value;
            OnPropertyChanged(nameof(ShowProbeSpheres));
            PrimalEditor_Avalonia.Native.NativeEngineBridge.PipelineSetProbeSphereDebug(value);
            PrimalEditor_Avalonia.Common.Logger.Info($"[GI] probe spheres → {value}");
        }
    }

    private const int MaxProbeDots = 2048;

    /// <summary>选中体积的 probe 格点世界坐标（格点序 x→y→z）。超上限时按
    /// 等距抽稀（保持空间覆盖），供 overlay 每帧绘制。</summary>
    public System.Collections.Generic.IReadOnlyList<(float x, float y, float z)> ComputeProbePoints()
    {
        if (!ShowProbes || !HasVolumes) return System.Array.Empty<(float, float, float)>();
        var mx = UseOriginOverride ? OriginX : 0f;
        var my = UseOriginOverride ? OriginY : 0f;
        var mz = UseOriginOverride ? OriginZ : 0f;
        ulong total = (ulong)CountX * CountY * CountZ;
        ulong stride = total > (ulong)MaxProbeDots
            ? (total + MaxProbeDots - 1) / MaxProbeDots : 1;
        var points = new System.Collections.Generic.List<(float, float, float)>(
            (int)Math.Min(total, (ulong)MaxProbeDots));
        ulong index = 0;
        for (uint z = 0; z < CountZ; ++z)
        for (uint y = 0; y < CountY; ++y)
        for (uint x = 0; x < CountX; ++x, ++index)
        {
            if (stride > 1 && index % stride != 0) continue;
            points.Add((
                mx + (x + 0.5f) * Spacing,
                my + (y + 0.5f) * Spacing,
                mz + (z + 0.5f) * Spacing));
        }
        return points;
    }

    /// <summary>视口线框用的 AABB（min = origin（若启用）否则世界原点；
    /// max = min + counts × spacing——与 GIVolumeGizmo 的 AABB 语义一致）。</summary>
    public ViewportPicker.AABB? SelectedBox
    {
        get
        {
            if (!HasVolumes) return null;
            var mx = UseOriginOverride ? OriginX : 0f;
            var my = UseOriginOverride ? OriginY : 0f;
            var mz = UseOriginOverride ? OriginZ : 0f;
            return new ViewportPicker.AABB(
                mx, my, mz,
                mx + CountX * Spacing, my + CountY * Spacing, mz + CountZ * Spacing);
        }
    }

    public void Refresh()
    {
        var count = NativeEngineBridge.GIVolumeOverrideCount();
        HasVolumes = count > 0;
        _volumeOptions = new System.Collections.Generic.List<string>();
        for (uint i = 0; i < count; ++i)
        {
            var buf = new byte[NativeEngineBridge.GIVolumeConfigCapacity];
            var dims = NativeEngineBridge.GIGetVolumeOverride(i, buf)
                ? $"{BitConverter.ToUInt32(buf, NativeEngineBridge.GIOffCountX)}×" +
                  $"{BitConverter.ToUInt32(buf, NativeEngineBridge.GIOffCountY)}×" +
                  $"{BitConverter.ToUInt32(buf, NativeEngineBridge.GIOffCountZ)}"
                : "?";
            _volumeOptions.Add($"Volume {i}   {dims}");
        }
        OnPropertyChanged(nameof(VolumeOptions));
        if (HasVolumes)
        {
            if (SelectedIndex >= count) _selectedIndexBacking = 0;
            LoadSelected();
            OnPropertyChanged(nameof(SelectedIndex));
            OnPropertyChanged(nameof(SelectedIndexOption));
        }
        OnPropertyChanged(nameof(SelectedBox));
    }

    private void LoadSelected()
    {
        var buf = new byte[NativeEngineBridge.GIVolumeConfigCapacity];
        if (!NativeEngineBridge.GIGetVolumeOverride(SelectedIndex, buf))
        {
            HasVolumes = NativeEngineBridge.GIVolumeOverrideCount() > 0;
            return;
        }
        CountX = BitConverter.ToUInt32(buf, NativeEngineBridge.GIOffCountX);
        CountY = BitConverter.ToUInt32(buf, NativeEngineBridge.GIOffCountY);
        CountZ = BitConverter.ToUInt32(buf, NativeEngineBridge.GIOffCountZ);
        Spacing = BitConverter.ToSingle(buf, NativeEngineBridge.GIOffSpacing);
        RaysPerProbe = BitConverter.ToUInt32(buf, NativeEngineBridge.GIOffRaysPerProbe);
        MaxDistance = BitConverter.ToSingle(buf, NativeEngineBridge.GIOffMaxDistance);
        OriginX = BitConverter.ToSingle(buf, NativeEngineBridge.GIOffOriginX);
        OriginY = BitConverter.ToSingle(buf, NativeEngineBridge.GIOffOriginY);
        OriginZ = BitConverter.ToSingle(buf, NativeEngineBridge.GIOffOriginZ);
        UseOriginOverride = buf[NativeEngineBridge.GIOffUseOrigin] != 0;
    }

    public void Apply()
    {
        var buf = new byte[NativeEngineBridge.GIVolumeConfigCapacity];
        BitConverter.GetBytes(CountX).CopyTo(buf, NativeEngineBridge.GIOffCountX);
        BitConverter.GetBytes(CountY).CopyTo(buf, NativeEngineBridge.GIOffCountY);
        BitConverter.GetBytes(CountZ).CopyTo(buf, NativeEngineBridge.GIOffCountZ);
        BitConverter.GetBytes(Spacing).CopyTo(buf, NativeEngineBridge.GIOffSpacing);
        BitConverter.GetBytes(RaysPerProbe).CopyTo(buf, NativeEngineBridge.GIOffRaysPerProbe);
        BitConverter.GetBytes(MaxDistance).CopyTo(buf, NativeEngineBridge.GIOffMaxDistance);
        BitConverter.GetBytes((uint)0).CopyTo(buf, NativeEngineBridge.GIOffBudgetDivisor);
        BitConverter.GetBytes(OriginX).CopyTo(buf, NativeEngineBridge.GIOffOriginX);
        BitConverter.GetBytes(OriginY).CopyTo(buf, NativeEngineBridge.GIOffOriginY);
        BitConverter.GetBytes(OriginZ).CopyTo(buf, NativeEngineBridge.GIOffOriginZ);
        buf[NativeEngineBridge.GIOffUseOrigin] = UseOriginOverride ? (byte)1 : (byte)0;
        if (NativeEngineBridge.GISetVolumeOverride(SelectedIndex, buf))
            Logger.Info($"[GI] volume {SelectedIndex} updated ({CountX}×{CountY}×{CountZ} @ {Spacing}m)");
    }

    public void Append()
    {
        if (NativeEngineBridge.GIAppendVolumeOverride())
        {
            Refresh();
            SelectedIndex = NativeEngineBridge.GIVolumeOverrideCount() - 1;
        }
    }

    public void Remove()
    {
        if (NativeEngineBridge.GIRemoveVolumeOverride(SelectedIndex))
        {
            Refresh();
        }
    }

    public void MaterializeDerived()
    {
        if (NativeEngineBridge.GIMaterializeDerivedVolumes())
        {
            Refresh();
        }
    }
}
