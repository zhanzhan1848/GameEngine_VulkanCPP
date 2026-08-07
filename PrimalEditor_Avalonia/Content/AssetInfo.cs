using System;
using System.IO;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Content;

public sealed class AssetInfo
{
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
}