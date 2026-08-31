using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Bridges static callsites (relay commands, toolbar handlers) back to
/// the currently-loaded project. The editor keeps only one project
/// open at a time, so a process-wide reference is sufficient and avoids
/// threading a Project reference through every command binding.
/// </summary>
public static class ProjectContextLocator
{
    public static Project? Current { get; private set; }

    public static void SetCurrent(Project? project) => Current = project;

    public static void Clear() => Current = null;
}
