using System.Collections.Generic;
using System.Linq;

namespace PrimalEditor_Avalonia.Content.Shaders;

public sealed class ShaderDocument
{
    public const int CurrentSchemaVersion = 1;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    public string Name { get; set; } = "Shader";

    public Dictionary<string, string> LanguageVariants { get; set; } = new();
    public Dictionary<string, string> EntryPoints { get; set; } = new();
    public List<string> Techniques { get; set; } = new();
    public List<string> Keywords { get; set; } = new();

    public bool IsValid =>
        SchemaVersion == CurrentSchemaVersion &&
        !string.IsNullOrWhiteSpace(Name) &&
        LanguageVariants.Count > 0 &&
        EntryPoints.ContainsKey("Vertex") &&
        EntryPoints.ContainsKey("Fragment");

    public IEnumerable<string> EnumerateDependencies() => LanguageVariants.Values.Where(p => !string.IsNullOrWhiteSpace(p));
}
