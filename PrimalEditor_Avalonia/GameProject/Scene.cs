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

    /// <summary>
    /// Lightmap 页路径（2026-10-19 计划 M4c）：场景级单一事实源。烘焙成功
    /// 后写入，随 .scene 持久化；场景激活同步时推给引擎（运行时按
    /// mesh_&lt;contentId&gt;_e&lt;entityId&gt; 命名约定自动匹配实体）。
    /// null = 无烘焙。
    /// </summary>
    private string? _lightmapPage;
    [JsonPropertyName("LightmapPage")]
    public string? LightmapPage
    {
        get => _lightmapPage;
        set
        {
            if (_lightmapPage != value)
            {
                _lightmapPage = value;
                OnPropertyChanged(nameof(LightmapPage));
                IsDirty = true;
            }
        }
    }

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
