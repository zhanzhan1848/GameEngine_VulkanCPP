using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Content;

public sealed class AssetInfo : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    [JsonPropertyName("Guid")]         public Guid Guid { get; set; } = Guid.NewGuid();
    [JsonPropertyName("Type")]         public AssetType Type { get; set; } = AssetType.Unknown;
    [JsonPropertyName("FullPath")]     public string FullPath { get; set; } = string.Empty;
    [JsonPropertyName("ImportDate")]   public DateTime ImportDate { get; set; } = DateTime.UtcNow;
    [JsonPropertyName("RegisterTime")] public DateTime RegisterTime { get; set; } = DateTime.UtcNow;
    [JsonPropertyName("Hash")]         public byte[]? Hash { get; set; }

    [JsonIgnore] public string FileName => Path.GetFileName(FullPath);
    [JsonIgnore] public string FileNameWithoutExt => Path.GetFileNameWithoutExtension(FullPath);
    [JsonIgnore] public string Extension => Path.GetExtension(FullPath).ToLowerInvariant();
    [JsonIgnore] public DateTime LastModified =>
        File.Exists(FullPath) ? File.GetLastWriteTime(FullPath) : DateTime.MinValue;
    [JsonIgnore] public bool IsDirectory =>
        Directory.Exists(FullPath) && !File.Exists(FullPath);
    [JsonIgnore] public long SizeBytes =>
        File.Exists(FullPath) ? new FileInfo(FullPath).Length : 0L;

    [JsonIgnore]
    public string SizeDisplay => SizeBytes switch
    {
        < 1024 => $"{SizeBytes} B",
        < 1024 * 1024 => $"{SizeBytes / 1024.0:F1} KB",
        _ => $"{SizeBytes / (1024.0 * 1024.0):F1} MB",
    };

    [JsonIgnore]
    public bool IsImage =>
        Type == AssetType.Texture &&
        (Extension == ".png" || Extension == ".jpg" ||
         Extension == ".jpeg" || Extension == ".bmp" ||
         Extension == ".texture");

    [JsonIgnore]
    public bool IsContainer => Type == AssetType.Model;

    /// <summary>B4 P4: particle-preset asset (.particle.json). Drives the
    /// content browser's "Apply to Selected Particle Entity" context menu.</summary>
    [JsonIgnore]
    public bool IsParticlePreset =>
        Type == AssetType.ParticlePreset ||
        Extension == ".particle.json";

    /// <summary>A8 源码控制: git working-tree status of this asset's file,
    /// refreshed by the source-control panel (None = untracked-by-git or
    /// unchanged). Drives the content browser status dot.</summary>
    [JsonIgnore]
    public Common.GitService.FileStatus GitStatus { get; private set; }
        = Common.GitService.FileStatus.None;

    /// <summary>Called by <see cref="ViewModels.SourceControlViewModel"/>
    /// after each status sweep; raises change notifications so the tile
    /// bindings re-evaluate.</summary>
    public void SetGitStatus(Common.GitService.FileStatus status)
    {
        if (GitStatus == status) return;
        GitStatus = status;
        OnPropertyChanged(nameof(GitStatus));
    }

    /// <summary>True when the entry is a folder on disk.</summary>
    [JsonIgnore]
    public bool IsFolder => Type == AssetType.Folder;

    [JsonIgnore]
    public List<AssetInfo> Children { get; set; } = new();

    private bool _isExpanded;
    private double _thumbnailColumnOffset;

    /// <summary>
    /// Whether the container's children are currently expanded in the
    /// browser UI. Tracked per instance — not persisted.
    /// </summary>
    [JsonIgnore]
    public bool IsExpanded
    {
        get => _isExpanded;
        set
        {
            if (_isExpanded == value) return;
            _isExpanded = value;
            OnPropertyChanged();
        }
    }

    /// <summary>
    /// Transient horizontal slot used by the thumbnail panel while this
    /// model's expanded content spans the full browser row.
    /// </summary>
    [JsonIgnore]
    public double ThumbnailColumnOffset
    {
        get => _thumbnailColumnOffset;
        internal set
        {
            if (Math.Abs(_thumbnailColumnOffset - value) < 0.01) return;
            _thumbnailColumnOffset = value;
            OnPropertyChanged();
        }
    }
}
