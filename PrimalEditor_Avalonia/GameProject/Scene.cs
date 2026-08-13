using System.Collections.ObjectModel;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.GameProject;

public class Scene : ViewModelBase, IDirtyTrackable
{
    public const string Extension = ".scene";

    public SceneGuid Guid { get; set; } = new SceneGuid(System.Guid.NewGuid());

    private string _name = "New Scene";
    private bool _isActive;
    private bool _isDirty;

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
    public SceneEnvironment Environment { get; } = new SceneEnvironment();

    /// <summary>Editor-only viewport camera, persisted per scene.</summary>
    [JsonIgnore]
    public EditorCameraState EditorCamera { get; } = new EditorCameraState();

    [JsonIgnore]
    public bool IsDirty
    {
        get => _isDirty;
        private set
        {
            if (_isDirty != value)
            {
                _isDirty = value;
                OnPropertyChanged(nameof(IsDirty));
            }
        }
    }

    public void MarkDirty() => IsDirty = true;
    public void ClearDirty() => IsDirty = false;

    [JsonIgnore]
    public Scene? ParentScene { get; set; }

    public Scene() { }

    public Scene(string name)
    {
        Name = name;
    }
}
