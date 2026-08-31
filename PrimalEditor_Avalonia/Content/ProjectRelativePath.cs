using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Content;

public readonly struct ProjectRelativePath : IEquatable<ProjectRelativePath>
{
    public string Value { get; }

    public ProjectRelativePath(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("Path cannot be empty", nameof(path));

        var normalized = path.Replace('\\', '/');
        if (normalized.StartsWith("/", StringComparison.Ordinal) ||
            (normalized.Length >= 2 && char.IsLetter(normalized[0]) && normalized[1] == ':'))
        {
            throw new ArgumentException($"Path cannot be rooted: {path}", nameof(path));
        }

        var segments = normalized.Split('/');
        if (segments.Any(segment => segment.Length == 0))
            throw new ArgumentException($"Path cannot contain empty segments: {path}", nameof(path));
        if (segments.Any(segment => segment == "."))
            throw new ArgumentException($"Path cannot contain current directory segments: {path}", nameof(path));
        if (segments.Any(segment => segment == ".."))
            throw new ArgumentException($"Path cannot contain parent directory traversal: {path}", nameof(path));

        Value = normalized;
    }

    public string Resolve(string projectRoot)
    {
        if (string.IsNullOrWhiteSpace(projectRoot))
            throw new ArgumentException("Project root cannot be empty", nameof(projectRoot));

        var normalizedRoot = Path.GetFullPath(projectRoot);
        var fullPath = Path.GetFullPath(Path.Combine(normalizedRoot, Value));
        var relative = Path.GetRelativePath(normalizedRoot, fullPath);
        if (Path.IsPathRooted(relative) || relative == ".." || relative.StartsWith($"..{Path.DirectorySeparatorChar}", StringComparison.Ordinal))
            throw new ArgumentException($"Resolved path '{fullPath}' is outside project root '{normalizedRoot}'");

        return fullPath;
    }

    public bool Equals(ProjectRelativePath other) =>
        string.Equals(Value, other.Value, StringComparison.Ordinal);

    public override bool Equals(object? obj) =>
        obj is ProjectRelativePath other && Equals(other);

    public override int GetHashCode() =>
        StringComparer.Ordinal.GetHashCode(Value ?? string.Empty);

    public static bool operator ==(ProjectRelativePath left, ProjectRelativePath right) => left.Equals(right);
    public static bool operator !=(ProjectRelativePath left, ProjectRelativePath right) => !left.Equals(right);

    public sealed class JsonConverter : JsonConverter<ProjectRelativePath>
    {
        public override ProjectRelativePath Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
        {
            var value = reader.GetString() ?? throw new JsonException("Project-relative path must be a string.");
            return new ProjectRelativePath(value);
        }

        public override void Write(Utf8JsonWriter writer, ProjectRelativePath value, JsonSerializerOptions options) =>
            writer.WriteStringValue(value.Value);
    }
}
