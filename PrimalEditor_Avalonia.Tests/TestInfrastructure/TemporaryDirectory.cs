using System;
using System.IO;

namespace PrimalEditor_Avalonia.Tests.TestInfrastructure;

public sealed class TemporaryDirectory : IDisposable
{
    private readonly string _tempDirectory;
    private bool _disposed;

    public string Path => _tempDirectory;

    public TemporaryDirectory()
    {
        _tempDirectory = System.IO.Path.Combine(System.IO.Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_tempDirectory);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        try
        {
            if (Directory.Exists(_tempDirectory))
            {
                Directory.Delete(_tempDirectory, recursive: true);
            }
        }
        catch
        {
        }

        _disposed = true;
    }
}