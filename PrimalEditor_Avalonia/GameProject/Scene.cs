using System.Collections.ObjectModel;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.GameProject;

public class Scene : ViewModelBase
{
    private string _name = "New Scene";
    private bool _isActive;

    [JsonPropertyName("Name")]
    public string Name
    {
        get => _name;
        set
        {
            if (_name != value)
            {
                _name = value;
                OnPropertyChanged(nameof(Name));
            }
        }
    }

    [JsonPropertyName("IsActive")]
    public bool IsActive
    {
        get => _isActive;
        set
        {
            if (_isActive != value)
            {
                _isActive = value;
                OnPropertyChanged(nameof(IsActive));
            }
        }
    }

    [JsonPropertyName("Entities")]
    public ObservableCollection<GameEntity> Entities { get; set; } = new();

    [JsonIgnore]
    public Scene? ParentScene { get; set; }

    public Scene() { }

    public Scene(string name)
    {
        Name = name;
    }
}