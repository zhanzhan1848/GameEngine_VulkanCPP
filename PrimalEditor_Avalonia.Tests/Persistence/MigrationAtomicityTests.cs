using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject.Migration;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public sealed class TempDirectoryFixture_v5 : IDisposable
{
    public string Path { get; } = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "primal_mig_" + Guid.NewGuid().ToString("N"));
    public TempDirectoryFixture_v5() => System.IO.Directory.CreateDirectory(Path);
    public void Dispose() { try { System.IO.Directory.Delete(Path, recursive: true); } catch { } }
}

public sealed class MigrationAtomicityTests : IClassFixture<TempDirectoryFixture_v5>
{
    private readonly TempDirectoryFixture_v5 _fixture;
    public MigrationAtomicityTests(TempDirectoryFixture_v5 f) => _fixture = f;

    private sealed class FaultyFileMover : IFileMover
    {
        public List<(string op, string src, string dst)> Calls { get; } = new();
        public int FailAfterMoves { get; set; } = int.MaxValue;
        public void Copy(string s, string d, bool o) { Calls.Add(("copy", s, d)); File.Copy(s, d, o); }
        public void Move(string s, string d, bool o)
        {
            Calls.Add(("move", s, d));
            if (Calls.Count(c => c.op == "move") > FailAfterMoves)
                throw new IOException("simulated move failure");
            File.Move(s, d, o);
        }
    }

    private sealed class RecoverableFileMover : IFileMover
    {
        public List<(string op, string src, string dst)> Calls { get; } = new();
        public int FailOnMoveCall { get; set; } = int.MaxValue;
        public void Copy(string s, string d, bool o) { Calls.Add(("copy", s, d)); File.Copy(s, d, o); }
        public void Move(string s, string d, bool o)
        {
            Calls.Add(("move", s, d));
            if (Calls.Count(c => c.op == "move") == FailOnMoveCall)
                throw new IOException($"simulated move failure on call {FailOnMoveCall}");
            File.Move(s, d, o);
        }
    }

    private static readonly string V1Fixture = Path.Combine(
        AppContext.BaseDirectory, "Fixtures", "Projects", "V1", "EmbeddedScenes.primal");

    [Fact]
    public void Migrate_WhenProjectFileMoveFails_AfterScenesMoved_SceneMovesAreRolledBack()
    {
        using var temp = new TemporaryProject();
        var projectFile = Path.Combine(temp.Path, "mytest.primal");
        File.Copy(V1Fixture, projectFile);

        // RecoverableFileMover fails ONLY on the second Move (which is the
        // project-file move); the scene Move and the rollback Move are
        // allowed to succeed, so we can verify the scenes came back.
        var recoverable = new RecoverableFileMover { FailOnMoveCall = 2 };
        var migration = new ProjectV1ToV2Migration(recoverable);

        var result = migration.Migrate(projectFile);

        var sceneFiles = Directory.Exists(temp.ScenesPath)
            ? Directory.GetFiles(temp.ScenesPath, "*.scene")
            : Array.Empty<string>();

        Console.WriteLine($"DEBUG scenes count={sceneFiles.Length}, projectFile exists={File.Exists(projectFile)}, err={result.ErrorMessage}");
        Console.WriteLine($"DEBUG mover calls: {string.Join(", ", recoverable.Calls.Select(c => c.op + "(" + Path.GetFileName(c.src) + "->" + Path.GetFileName(c.dst) + ")"))}");

        Assert.False(result.Success);
        Assert.True(File.Exists(projectFile));
        Assert.Empty(sceneFiles);
    }

    [Fact]
    public void Migrate_WhenEveryMoveFails_ProjectIsUntouched()
    {
        using var temp = new TemporaryProject();
        // Use CopyFixture but rename fixture to a temp-specific name so the
        // fixture file itself stays intact across this test.
        var projectFile = Path.Combine(temp.Path, "mytest.primal");
        File.Copy(V1Fixture, projectFile);

        var faulty = new FaultyFileMover { FailAfterMoves = 0 };
        var migration = new ProjectV1ToV2Migration(faulty);

        var result = migration.Migrate(projectFile);

        var filesInDir = Directory.GetFiles(temp.Path);
        var bakFiles = filesInDir.Where(f => f.Contains(".v1.")).ToArray();
        Console.WriteLine($"DEBUG result.Success={result.Success}, projectFilePath exists={File.Exists(projectFile)}, all files: [{string.Join(",", filesInDir.Select(Path.GetFileName))}], bak count={bakFiles.Length}, err={result.ErrorMessage}");

        Assert.False(result.Success);
        Assert.True(File.Exists(projectFile));
    }
}
