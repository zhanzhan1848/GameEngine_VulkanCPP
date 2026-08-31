using System;
using Avalonia.Controls;
using Avalonia.Input;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Editors.Material;

public partial class MaterialPreviewView : UserControl
{
    private Avalonia.Point _lastPointer;
    private bool _dragging;

    public MaterialPreviewView()
    {
        InitializeComponent();

        // Drive the preview timer off the view's visual lifetime so we
        // don't burn CPU when the editor is closed or in the background.
        // AttachedToVisualTree fires once when the view is added to a window;
        // DetachedFromVisualTree fires on close / dock reparent.
        AttachedToVisualTree += (_, _) => StartPreview();
        DetachedFromVisualTree += (_, _) => StopPreview();
    }

    private void StartPreview()
    {
        try
        {
            if (DataContext is MaterialPreviewViewModel vm) vm.Start();
        }
        catch (Exception ex)
        {
            Logger.Error("MaterialPreviewView.StartPreview failed", ex);
        }
    }

    private void StopPreview()
    {
        try
        {
            if (DataContext is MaterialPreviewViewModel vm) vm.Stop();
        }
        catch (Exception ex)
        {
            Logger.Error("MaterialPreviewView.StopPreview failed", ex);
        }
    }

    private void InitializeComponent() => Avalonia.Markup.Xaml.AvaloniaXamlLoader.Load(this);

    /// <summary>
    /// Begin orbit-drag. Capture the pointer so the cursor can leave the
    /// Image control mid-drag (e.g. user drags fast) and we still receive
    /// Move/Release.
    /// </summary>
    private void OnImagePointerPressed(object? sender, PointerPressedEventArgs e)
    {
        try
        {
            if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            _lastPointer = e.GetPosition(this);
            _dragging = true;
            e.Pointer.Capture(this);
            e.Handled = true;
        }
        catch (Exception ex)
        {
            Logger.Error("MaterialPreviewView.OnImagePointerPressed failed", ex);
        }
    }

    private void OnImagePointerMoved(object? sender, PointerEventArgs e)
    {
        try
        {
            if (!_dragging) return;
            if (DataContext is not MaterialPreviewViewModel vm) return;
            var p = e.GetPosition(this);
            var dx = p.X - _lastPointer.X;
            var dy = p.Y - _lastPointer.Y;
            _lastPointer = p;
            vm.OnDragDelta(dx, dy);
        }
        catch (Exception ex)
        {
            Logger.Error("MaterialPreviewView.OnImagePointerMoved failed", ex);
        }
    }

    private void OnImagePointerReleased(object? sender, PointerReleasedEventArgs e)
    {
        try
        {
            _dragging = false;
            e.Pointer.Capture(null);
        }
        catch (Exception ex)
        {
            Logger.Error("MaterialPreviewView.OnImagePointerReleased failed", ex);
        }
    }
}