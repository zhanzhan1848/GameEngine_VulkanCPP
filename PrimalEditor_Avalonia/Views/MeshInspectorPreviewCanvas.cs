using System;
using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Editors.MeshAssetInspector;
using PrimalEditor_Avalonia.Views.Engine3D;

namespace PrimalEditor_Avalonia.Views;

/// <summary>
/// Wireframe preview surface for the MeshAssetInspector window. Draws
/// the active LOD through the pure-CPU <see cref="WireframeRenderer"/>,
/// auto-fitted to the geometry's bounds (the side-pane canvas assumes
/// unit-scaled assets, cooked meshes do not). The spin timer lives here
/// rather than in the view model so it starts/stops with the visual
/// tree — closing the editor window tears the canvas down and the
/// timer with it, no dispose plumbing needed.
/// </summary>
public class MeshInspectorPreviewCanvas : Control
{
    private DispatcherTimer? _spinTimer;
    private float _yaw;

    // Bounds of the LOD currently drawn: center + radius drive the
    // camera fit. Cached per LOD reference — recomputing an AABB over
    // every vertex on each spin tick would dominate the frame.
    private MeshLOD? _fitLod;
    private float _centerX, _centerY, _centerZ, _radius;

    public MeshInspectorPreviewCanvas()
    {
        ClipToBounds = true;
        AttachedToVisualTree += (_, _) => StartSpin();
        DetachedFromVisualTree += (_, _) => StopSpin();
        DataContextChanged += (_, _) => { _fitLod = null; InvalidateVisual(); };
    }

    private MeshAssetInspectorViewModel? Vm => DataContext as MeshAssetInspectorViewModel;

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        // DataContext is the view model; repaint when the drawn content
        // changes (LOD switch, submesh highlight). Property-change
        // subscription alone misses the first DataContext swap.
        if (e.Property.Name == nameof(DataContext))
        {
            if (e.OldValue is MeshAssetInspectorViewModel oldVm)
                oldVm.PropertyChanged -= OnVmPropertyChanged;
            if (e.NewValue is MeshAssetInspectorViewModel newVm)
                newVm.PropertyChanged += OnVmPropertyChanged;
        }
    }

    private void OnVmPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(MeshAssetInspectorViewModel.ActiveLodIndex)
            or nameof(MeshAssetInspectorViewModel.SelectedSubmeshIndex))
        {
            InvalidateVisual();
        }
    }

    private void StartSpin()
    {
        if (_spinTimer != null) return;
        _spinTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(40) };
        _spinTimer.Tick += (_, _) =>
        {
            _yaw = (_yaw + 0.02f) % (MathF.PI * 2f);
            InvalidateVisual();
        };
        _spinTimer.Start();
    }

    private void StopSpin()
    {
        _spinTimer?.Stop();
        _spinTimer = null;
    }

    public override void Render(DrawingContext context)
    {
        context.FillRectangle(Brushes.Black, new Rect(Bounds.Size));

        var vm = Vm;
        var lodIndex = vm?.ActiveLodIndex ?? 0;
        var lods = vm?.Geometry.LODs;
        if (vm == null || lods == null || lods.Count == 0) return;
        if (lodIndex < 0 || lodIndex >= lods.Count) return;
        var lod = lods[lodIndex];

        var w = (float)Bounds.Width;
        var h = (float)Bounds.Height;
        if (w < 1f || h < 1f) return;

        if (!ReferenceEquals(_fitLod, lod)) _fitLod = ComputeFit(lod);

        if (_radius <= 0f) return;
        var distance = _radius * 2.2f;

        WireframeRenderer.Draw(context, lod, new WireframeRenderer.ViewState(
            yaw: _yaw,
            pitch: 0.35f,
            distance: distance,
            tx: _centerX, ty: _centerY, tz: _centerZ,
            vw: w, vh: h),
            submeshIndex: vm!.SelectedSubmeshIndex);
    }

    private MeshLOD ComputeFit(MeshLOD lod)
    {
        float minX = float.MaxValue, minY = float.MaxValue, minZ = float.MaxValue;
        float maxX = float.MinValue, maxY = float.MinValue, maxZ = float.MinValue;

        foreach (var mesh in lod.Meshes)
        {
            if (mesh.VertexCount <= 0 || mesh.Positions.Length < mesh.VertexCount * 3 * sizeof(float)) continue;
            var pos = mesh.GetPositionsAsFloat3();
            for (var i = 0; i < pos.Length; i += 3)
            {
                var x = pos[i]; var y = pos[i + 1]; var z = pos[i + 2];
                if (x < minX) minX = x;
                if (y < minY) minY = y;
                if (z < minZ) minZ = z;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
                if (z > maxZ) maxZ = z;
            }
        }

        if (minX > maxX)
        {
            _centerX = _centerY = _centerZ = 0f;
            _radius = 0f;
        }
        else
        {
            _centerX = (minX + maxX) * 0.5f;
            _centerY = (minY + maxY) * 0.5f;
            _centerZ = (minZ + maxZ) * 0.5f;
            var dx = maxX - minX; var dy = maxY - minY; var dz = maxZ - minZ;
            _radius = 0.5f * MathF.Sqrt(dx * dx + dy * dy + dz * dz);
            if (float.IsNaN(_radius) || float.IsInfinity(_radius) || _radius <= 0f) _radius = 1f;
        }
        return lod;
    }
}
