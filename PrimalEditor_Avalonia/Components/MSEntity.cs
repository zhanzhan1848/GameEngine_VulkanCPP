using System.Collections.ObjectModel;
using System.Linq;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

public class MSEntity : ViewModelBase
{
    private System.Guid _guid;

    public System.Guid Guid { get => _guid; set => _guid = value; }

    public ObservableCollection<GameEntity> SelectedEntities { get; } = new();

    public string Name
    {
        get
        {
            if (SelectedEntities.Count == 0) return string.Empty;
            var first = SelectedEntities[0].Name;
            return SelectedEntities.All(e => e.Name == first) ? first : "<multiple>";
        }
    }

    public bool IsEnabled
    {
        get => SelectedEntities.Count > 0 && SelectedEntities.All(e => e.IsEnabled);
        set
        {
            foreach (var e in SelectedEntities) e.IsEnabled = value;
            OnPropertyChanged();
        }
    }

    public MSEntity() : this(System.Guid.NewGuid()) { }

    public MSEntity(System.Guid guid)
    {
        _guid = guid;
    }
}