namespace PrimalEditor_Avalonia.GameProject.Migration;

public interface IProjectMigration
{
    int SourceVersion { get; }
    int TargetVersion { get; }
    ProjectMigrationResult Migrate(string projectFilePath);
}

public readonly record struct ProjectMigrationResult(bool Success, bool Migrated, string? ErrorMessage = null)
{
    public static ProjectMigrationResult NotRequired() => new(true, false);
    public static ProjectMigrationResult Completed() => new(true, true);
    public static ProjectMigrationResult Failed(string error) => new(false, false, error);
}
