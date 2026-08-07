using System.ComponentModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using PrimalEditor_Avalonia.ViewModels;
using PrimalEditor_Avalonia.Views.Engine3D;

namespace PrimalEditor_Avalonia.Views;

/// <summary>
/// Side-pane mesh preview surface. Forwards drawing to the pure-CPU
/// <see>WireframeRenderer</see> and reads the camera yaw from the
/// view model so the spin animation in
/// <see>ContentBrowserPreviewViewModel</see> drives the rotation.
/// </summary>
public class MeshPreviewCanvas : Control
{
    public static readonly StyledProperty<ContentBrowserPreviewViewModel?> ViewModelProperty =
        AvaloniaProperty.Register<MeshPreviewCanvas, ContentBrowserPreviewViewModel?>(nameof(ViewModel));

    private ContentBrowserPreviewViewModel? _attached;

    public ContentBrowserPreviewViewModel? ViewModel
    {
        get => GetValue(ViewModelProperty);
        set
        {
            var old = _attached;
            if (old != null) old.PropertyChanged -= OnViewModelPropertyChanged;
            _attached = value;
            if (value != null) value.PropertyChanged += OnViewModelPropertyChanged;
            SetValue(ViewModelProperty, value);
            InvalidateVisual();
        }
    }

    public MeshPreviewCanvas()
    {
        ClipToBounds = true;
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        // MeshYaw ticks every frame from the spin timer; PreviewLOD
        // flips on selection change. Both need a repaint.
        if (e.PropertyName is nameof(ContentBrowserPreviewViewModel.MeshYaw)
            or nameof(ContentBrowserPreviewViewModel.PreviewLOD))
        {
            InvalidateVisual();
        }
    }

    public override void Render(DrawingContext context)
    {
        context.FillRectangle(Brushes.Black, new Rect(Bounds.Size));

        var vm = ViewModel;
        var lod = vm?.PreviewLOD;
        if (lod == null) return;

        var w = (float)Bounds.Width;
        var h = (float)Bounds.Height;
        if (w < 1f || h < 1f) return;

        var view = new WireframeRenderer.ViewState(
            yaw: vm!.MeshYaw,
            pitch: 0.35f,
            distance: 3f,
            tx: 0f, ty: 0f, tz: 0f,
            vw: w, vh: h);
        WireframeRenderer.Draw(context, lod, view);
    }
}