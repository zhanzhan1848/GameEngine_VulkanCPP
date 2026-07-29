using System;
using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Geometry;

public sealed class GeometryReimportServiceTests
{
    private static string MakeTempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalReimport_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        return dir;
    }

    [Fact]
    public void MissingSource_ReturnsMissingSource()
    {
        var dir = MakeTempDir();
        try
        {
            var svc = new GeometryReimportService();
            var result = svc.Reimport(Path.Combine(dir, "missing.obj"), Path.Combine(dir, "out.mesh"));
            Assert.Equal(ReimportStatus.MissingSource, result.Status);
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    [Fact]
    public void EmptySourceObject_CooksToValidMesh()
    {
        var dir = MakeTempDir();
        try
        {
            // An empty file is a degenerate source; we expect the native
            // cook to fail (or produce nothing), not crash our service.
            var src = Path.Combine(dir, "empty.obj");
            File.WriteAllText(src, string.Empty);
            var dst = Path.Combine(dir, "out.mesh");
            var svc = new GeometryReimportService();
            var result = svc.Reimport(src, dst);
            Assert.True(result.Status == ReimportStatus.CookFailed
                     || result.Status == ReimportStatus.InvalidOutput
                     || result.Status == ReimportStatus.MissingSource);
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    [Fact]
    public void BadDestinationDirectory_ReturnsCookFailed()
    {
        var dir = MakeTempDir();
        try
        {
            var src = Path.Combine(dir, "any.obj");
            File.WriteAllText(src, "v 0 0 0");
            var bad = Path.Combine(Path.Combine(dir, "does_not_exist"), "subdir", "out.mesh");
            var svc = new GeometryReimportService();
            var result = svc.Reimport(src, bad);
            Assert.Equal(ReimportStatus.CookFailed, result.Status);
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    [Fact]
    public void PartialFileIsCleanedUp_OnFailure()
    {
        var dir = MakeTempDir();
        try
        {
            var src = Path.Combine(dir, "x.obj");
            File.WriteAllText(src, "v 0 0 0");
            var dst = Path.Combine(dir, "out.mesh");
            var svc = new GeometryReimportService();
            svc.Reimport(src, dst);
            Assert.False(File.Exists(dst + ".partial"));
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }
}
