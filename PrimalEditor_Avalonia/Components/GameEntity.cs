using System.Collections.ObjectModel;
using System.Linq;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

public class GameEntity : ViewModelBase
{
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

    /// <summary>
    /// Optional absolute path to an asset (e.g. <c>Content/Models/foo.mesh</c>)
    /// that this entity is bound to. When the scene is reopened, the
    /// editor re-imports this asset through the engine and reattaches
    /// the resulting mesh content id to the entity via
    /// <see cref="WorldEditorViewModel.LoadEntity"/>. Empty string for
    /// entities that aren't bound to an asset (the typical case).
    /// </summary>
    [JsonPropertyName("AssetPath")]
    public string AssetPath { get; set; } = string.Empty;

    [JsonPropertyName("Components")]
    public ObservableCollection<Component> Components { get; set; } = new();

    [JsonPropertyName("Children")]
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