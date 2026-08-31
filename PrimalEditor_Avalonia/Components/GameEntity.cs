using System.Collections.ObjectModel;
using System.Linq;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

public class GameEntity : ViewModelBase
{
    public EntityGuid EntityGuid { get; set; } = new EntityGuid(System.Guid.NewGuid());

    private string _name = "Entity";
    private bool _isEnabled = true;
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

    [JsonPropertyName("IsEnabled")]
    public bool IsEnabled
    {
        get => _isEnabled;
        set
        {
            if (_isEnabled != value)
            {
                _isEnabled = value;
                OnPropertyChanged(nameof(IsEnabled));
            }
        }
    }

    [JsonIgnore]
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

    [JsonIgnore]
    public string AssetPath { get; set; } = string.Empty;

    [JsonPropertyName("Components")]
    public ObservableCollection<Component> Components { get; set; } = new();

    [JsonIgnore]
    public ObservableCollection<GameEntity> Children { get; set; } = new();

    [JsonIgnore]
    public Scene? ParentScene { get; set; }

    [JsonIgnore]
    public GameEntity? Parent { get; set; }

    public GameEntity() { }

    public GameEntity(string name)
    {
        Name = name;
    }

    public T? GetComponent<T>() where T : Component =>
        Components.OfType<T>().FirstOrDefault();

    public Transform? GetTransform() => GetComponent<Transform>();
}