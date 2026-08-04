using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.GameProject;

public class Project : ViewModelBase, IDirtyTrackable
{
    public static string Extension { get; } = ".primal";
    public static int SchemaVersion { get; } = 2;

    public ProjectGuid Guid { get; set; } = new ProjectGuid(System.Guid.NewGuid());

    private string _name = "New Project";
    private string _path = string.Empty;
    private int _buildConfig;
    private Scene? _activeScene;
    private readonly ObservableCollection<Scene> _scenes = new();
    private bool _isDirty;

    [JsonPropertyName("SchemaVersion")]
    public int CurrentSchemaVersion { get; set; } = SchemaVersion;

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

    [JsonIgnore]
    public string Path
    {
        get => _path;
        set
        {
            if (_path != value)
            {
                _path = value;
                OnPropertyChanged(nameof(Path));
                OnPropertyChanged(nameof(FullPath));
                OnPropertyChanged(nameof(Solution));
                OnPropertyChanged(nameof(ContentPath));
                OnPropertyChanged(nameof(ScenesPath));
            }
        }
    }

    [JsonIgnore]
    public string FullPath => System.IO.Path.Combine(Path, Name + Extension);

    [JsonIgnore]
    public string Solution => System.IO.Path.Combine(Path, Name + ".sln");

    [JsonIgnore]
    public string ContentPath => System.IO.Path.Combine(Path, "Content");

    [JsonIgnore]
    public string ScenesPath => System.IO.Path.Combine(Path, "Scenes");

    [JsonIgnore]
    public string TempFolder => System.IO.Path.Combine(Path, ".Primal", "Temp");

    [JsonPropertyName("BuildConfig")]
    public int BuildConfig
    {
        get => _buildConfig;
        set
        {
            if (_buildConfig != value)
            {
                _buildConfig = value;
                OnPropertyChanged(nameof(BuildConfig));
                OnPropertyChanged(nameof(BuildConfigName));
            }
        }
    }

    [JsonIgnore]
    public static readonly string[] BuildConfigurationNames = { "Debug", "DebugEditor", "Release", "ReleaseEditor" };

    [JsonIgnore]
    public string BuildConfigName
    {
        get => BuildConfigurationNames[System.Math.Clamp(BuildConfig, 0, BuildConfigurationNames.Length - 1)];
        set
        {
            var idx = System.Array.IndexOf(BuildConfigurationNames, value);
            if (idx >= 0) BuildConfig = idx;
        }
    }

    [JsonIgnore]
    public ObservableCollection<Scene> Scenes
    {
        get => _scenes;
        set
        {
            if (_scenes != value)
            {
                _scenes.Clear();
                if (value != null)
                {
                    foreach (var scene in value)
                    {
                        _scenes.Add(scene);
                    }
                }
                OnPropertyChanged(nameof(Scenes));
            }
        }
    }

    [JsonIgnore]
    public Scene? ActiveScene
    {
        get => _activeScene;
        set
        {
            if (_activeScene != value)
            {
                _activeScene = value;
                OnPropertyChanged(nameof(ActiveScene));
            }
        }
    }

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

    public Project() { }

    public static Project CreateNew(string name, string path)
    {
        return new Project
        {
            Name = name,
            Path = path,
            BuildConfig = 0,
        };
    }
}