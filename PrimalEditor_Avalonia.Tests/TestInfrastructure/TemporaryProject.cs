using System;
using System.IO;
using System.Text.Json;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Tests.TestInfrastructure;

/// <summary>
/// Helper for creating temporary project directories for testing.
/// Automatically cleans up the temporary directory when disposed.
/// </summary>
public sealed class TemporaryProject : IDisposable
{
    private readonly string _tempDirectory;
    private bool _disposed;

    /// <summary>
    /// Gets the path to the temporary project directory.
    /// </summary>
    public string Path => _tempDirectory;

    /// <summary>
    /// Gets the full path to the project file.
    /// </summary>
    public string ProjectFilePath => System.IO.Path.Combine(_tempDirectory, "TestProject.primal");

    /// <summary>
    /// Gets the full path to the Content directory.
    /// </summary>
    public string ContentPath => System.IO.Path.Combine(_tempDirectory, "Content");

    /// <summary>
    /// Gets the full path to the Scenes directory.
    /// </summary>
    public string ScenesPath => System.IO.Path.Combine(_tempDirectory, "Scenes");

    /// <summary>
    /// Creates a new temporary project directory.
    /// </summary>
    public TemporaryProject()
    {
        _tempDirectory = System.IO.Path.Combine(
            System.IO.Path.GetTempPath(),
            $"PrimalTest_{Guid.NewGuid():N}"
        );

        Directory.CreateDirectory(_tempDirectory);
        Directory.CreateDirectory(ContentPath);
        Directory.CreateDirectory(ScenesPath);
    }

    /// <summary>
    /// Copies a fixture project file to the temporary directory.
    /// </summary>
    /// <param name="fixturePath">Path to the fixture project file.</param>
    /// <param name="newName">Optional new name for the project file.</param>
    /// <returns>The path to the copied project file.</returns>
    public string CopyFixture(string fixturePath, string? newName = null)
    {
        if (!File.Exists(fixturePath))
        {
            throw new FileNotFoundException($"Fixture not found: {fixturePath}");
        }

        var destPath = newName != null
            ? System.IO.Path.Combine(_tempDirectory, newName)
            : ProjectFilePath;

        File.Copy(fixturePath, destPath, overwrite: true);
        return destPath;
    }

    /// <summary>
    /// Loads a project from the temporary directory.
    /// </summary>
    /// <returns>The loaded project, or null if loading failed.</returns>
    public Project? LoadProject()
    {
        return Serializer.LoadProject(ProjectFilePath);
    }

    /// <summary>
    /// Saves a project to the temporary directory.
    /// </summary>
    /// <param name="project">The project to save.</param>
    public void SaveProject(Project project)
    {
        Serializer.SaveProject(project, ProjectFilePath);
    }

    /// <summary>
    /// Reads the raw JSON content of the project file.
    /// </summary>
    /// <returns>The JSON content as a string.</returns>
    public string ReadProjectJson()
    {
        return File.ReadAllText(ProjectFilePath);
    }

    /// <summary>
    /// Parses the project file as JSON for inspection.
    /// </summary>
    /// <returns>A JsonElement representing the project.</returns>
    public JsonElement ParseProjectJson()
    {
        var json = ReadProjectJson();
        return JsonSerializer.Deserialize<JsonElement>(json);
    }

    /// <summary>
    /// Creates a subdirectory within the Content folder.
    /// </summary>
    /// <param name="relativePath">The relative path within Content.</param>
    /// <returns>The full path to the created directory.</returns>
    public string CreateContentDirectory(string relativePath)
    {
        var fullPath = System.IO.Path.Combine(ContentPath, relativePath);
        Directory.CreateDirectory(fullPath);
        return fullPath;
    }

    /// <summary>
    /// Creates a file within the Content folder.
    /// </summary>
    /// <param name="relativePath">The relative path within Content.</param>
    /// <param name="content">The file content.</param>
    /// <returns>The full path to the created file.</returns>
    public string CreateContentFile(string relativePath, string content)
    {
        var fullPath = System.IO.Path.Combine(ContentPath, relativePath);
        var directory = System.IO.Path.GetDirectoryName(fullPath);

        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        File.WriteAllText(fullPath, content);
        return fullPath;
    }

    public void Dispose()
    {
        if (_disposed) return;

        try
        {
            if (Directory.Exists(_tempDirectory))
            {
                Directory.Delete(_tempDirectory, recursive: true);
            }
        }
        catch
        {
            // Best effort cleanup - don't throw from Dispose
        }

        _disposed = true;
        GC.SuppressFinalize(this);
    }
}