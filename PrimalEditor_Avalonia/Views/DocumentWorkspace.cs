using Avalonia;
using Avalonia.Controls;
using Dock.Model.Core;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

/// <summary>Document host. Tool tabs belong inside WorldEditorView's bottom panel.</summary>
public sealed class DocumentWorkspace : Grid
{
    public static readonly StyledProperty<IDockable?> ActiveDocumentProperty =
        AvaloniaProperty.Register<DocumentWorkspace, IDockable?>(nameof(ActiveDocument));

    public IDockable? ActiveDocument
    {
        get => GetValue(ActiveDocumentProperty);
        set => SetValue(ActiveDocumentProperty, value);
    }

    private WorldEditorView? _world;

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property != ActiveDocumentProperty) return;
        Children.Clear();
        if (ActiveDocument is WorldEditorViewModel worldVm)
        {
            if (_world == null || !ReferenceEquals(_world.DataContext, worldVm))
                _world = new WorldEditorView { DataContext = worldVm };
            Children.Add(_world);
        }
        else
            Children.Add(new ContentControl { Content = ActiveDocument });
    }
}
