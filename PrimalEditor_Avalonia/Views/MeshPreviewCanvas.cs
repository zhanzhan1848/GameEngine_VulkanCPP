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
        // Subscribe/unsubscribe around the visual tree lifetime. The
        // template visual tree is torn down and rebuilt whenever the
        // previewed type changes (e.g. switching from a mesh wireframe
        // to a texture bitmap), so the listener must follow the visual
        // attachment rather than the static DataContext reference.
        AttachedToVisualTree += (_, _) => AttachListener();
        DetachedFromVisualTree += (_, _) => DetachListener();
        DataContextChanged += (_, _) => AttachListener();
    }

    private void AttachListener()
    {
        var newVm = DataContext as ContentBrowserPreviewViewModel;
        if (ReferenceEquals(newVm, _attached)) return;
        if (_attached != null) _attached.PropertyChanged -= OnViewModelPropertyChanged;
        _attached = newVm;
        if (newVm != null) newVm.PropertyChanged += OnViewModelPropertyChanged;
        InvalidateVisual();
    }

    private void DetachListener()
    {
        if (_attached == null) return;
        _attached.PropertyChanged -= OnViewModelPropertyChanged;
        _attached = null;
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        // MeshYaw ticks every frame from the spin timer; PreviewLOD
        // flips on selection change; SelectedSubmeshIndex changes
        // which submesh inside the LOD is highlighted.
        if (e.PropertyName is nameof(ContentBrowserPreviewViewModel.MeshYaw)
            or nameof(ContentBrowserPreviewViewModel.PreviewLOD)
            or nameof(ContentBrowserPreviewViewModel.SelectedSubmeshIndex))
        {
            InvalidateVisual();
        }
    }

    public override void Render(DrawingContext context)
    {
        context.FillRectangle(Brushes.Black, new Rect(Bounds.Size));

        // Read the view model from DataContext directly — the
        // StyledProperty is set via {Binding} but Avalonia 11 may not
        // fire the setter when the reference does not change, leaving
        // this canvas pointing at a stale VM. DataContext inheritance
        // is reliable and reflects the live PreviewViewModel.
        var vm = DataContext as ContentBrowserPreviewViewModel;
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
        WireframeRenderer.Draw(context, lod, view, submeshIndex: vm.SelectedSubmeshIndex);

        // Do NOT call InvalidateVisual() here. Avalonia's render pass
        // forbids marking a visual dirty while it is being drawn (it
        // throws "Visual was invalidated during the render pass"). The
        // spin timer's MeshYaw PropertyChanged handler repaints the
        // canvas via the listener attached in AttachListener().
    }
}