using System;

namespace PrimalEditor_Avalonia.Editors.Geometry;

/// <summary>
/// Outcome of a <see cref="GeometryReimportService.Reimport"/> call.
/// </summary>
public enum ReimportStatus
{
    Success,
    MissingSource,
    CookFailed,
    InvalidOutput,
    NoChange,
}

public sealed class ReimportResult
{
    public ReimportStatus Status { get; }
    public string? Message { get; }
    public string? NewMeshPath { get; }
    public long? NewSize { get; }
    public ReimportResult(ReimportStatus status, string? message = null, string? newMeshPath = null, long? newSize = null)
    {
        Status = status;
        Message = message;
        NewMeshPath = newMeshPath;
        NewSize = newSize;
    }
}
