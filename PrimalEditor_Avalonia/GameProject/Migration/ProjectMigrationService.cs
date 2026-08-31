using System.IO;
using System.Text.Json;
using PrimalEditor_Avalonia.GameProject.Documents;

namespace PrimalEditor_Avalonia.GameProject.Migration;

public sealed class ProjectMigrationService
{
    public ProjectMigrationResult MigrateIfNeeded(string projectFilePath)
    {
        if (!File.Exists(projectFilePath)) return ProjectMigrationResult.Failed("Project file does not exist.");
        try
        {
            using var json = JsonDocument.Parse(File.ReadAllText(projectFilePath));
            var version = json.RootElement.TryGetProperty("SchemaVersion", out var value) ? value.GetInt32() : 1;
            if (version == ProjectDocument.CurrentSchemaVersion) return ProjectMigrationResult.NotRequired();
            if (version != 1) return ProjectMigrationResult.Failed($"Unsupported project schema version: {version}");
            return new ProjectV1ToV2Migration().Migrate(projectFilePath);
        }
        catch (JsonException ex)
        {
            return ProjectMigrationResult.Failed(ex.Message);
        }
    }
}
