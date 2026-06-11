using System.Collections.Generic;
using System.IO;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.GameProject;

public class ProjectTemplate
{
    [JsonPropertyName("Name")]
    public string Name { get; set; } = "Empty Project";

    [JsonPropertyName("Description")]
    public string Description { get; set; } = "A new empty project";

    [JsonPropertyName("TemplatePath")]
    public string TemplatePath { get; set; } = string.Empty;

    [JsonPropertyName("IconPath")]
    public string? IconPath { get; set; }

    public ProjectTemplate() { }

    public ProjectTemplate(string name, string description, string templatePath, string? iconPath = null)
    {
        Name = name;
        Description = description;
        TemplatePath = templatePath;
        IconPath = iconPath;
    }
}

public static class ProjectTemplates
{
    public static List<ProjectTemplate> GetDefaultTemplates()
    {
        return new List<ProjectTemplate>
        {
            new("Empty Project", "A new empty project with no scenes", "EmptyProject"),
            new("3D Scene", "A simple 3D scene with a single entity", "3DScene"),
            new("2D Scene", "A simple 2D scene template", "2DScene"),
            new("UI Scene", "A scene with UI elements", "UIScene"),
            new("Physics Scene", "A scene with physics-enabled entities", "PhysicsScene"),
        };
    }
}