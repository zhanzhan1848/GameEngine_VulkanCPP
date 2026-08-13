using System.Collections.ObjectModel;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Lightweight metadata attached to any entity: <see cref="IsPlayer"/>
/// tag (for the main character or player avatar), free-form
/// <see cref="Tags"/> strings, and a <see cref="Description"/>. The
/// metadata has no native engine side — it lives only on the C# side
/// for game-script filtering ("is this the player?", "is this a
/// pickup?") and editor convenience. Persists via JsonDerivedType.
/// </summary>
public sealed class EntityMetadata : Component
{
    private bool _isPlayer;
    private string _description = string.Empty;

    [JsonPropertyName("IsPlayer")]
    public bool IsPlayer
    {
        get => _isPlayer;
        set { if (_isPlayer != value) { _isPlayer = value; OnPropertyChanged(nameof(IsPlayer)); } }
    }

    [JsonPropertyName("Description")]
    public string Description
    {
        get => _description;
        set { if (_description != value) { _description = value; OnPropertyChanged(nameof(Description)); } }
    }

    [JsonPropertyName("Tags")]
    public ObservableCollection<string> Tags { get; set; } = new();

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSEntityMetadata(msEntity);
}
