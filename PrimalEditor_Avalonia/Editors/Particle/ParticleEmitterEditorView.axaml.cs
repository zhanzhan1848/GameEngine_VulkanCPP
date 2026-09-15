using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Editors.Particle;

public partial class ParticleEmitterEditorView : UserControl
{
    public ParticleEmitterEditorView()
    {
        InitializeComponent();
        // CurveEdited is a plain Action (shared with the live-entity curve
        // editor window), so subscribe here instead of via an XAML event.
        var editor = this.FindControl<EmitterCurveEditor>("CurveEditor");
        if (editor != null)
            editor.CurveEdited += (_, _) =>
                (DataContext as ParticleEmitterEditorViewModel)?.NotifyCurveEdited();
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnSaveClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is ParticleEmitterEditorViewModel vm) vm.Save();
    }

    private void OnDiscardClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is ParticleEmitterEditorViewModel vm) vm.Discard();
    }
}
