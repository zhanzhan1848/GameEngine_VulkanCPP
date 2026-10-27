using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Cli;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Cli;

/// <summary>
/// CI 接口快照门禁（CLI 规划 §3.2/§6.2）：命令面的任何变化（新增 verb、
/// 参数、描述、示例）都会改变 $version 与序列化输出，本测试即红——
/// 强制接口变更在 PR 中可见。有意变更时更新快照并提交：
///   PRIMAL_UPDATE_SCHEMA=1 dotnet test --filter CliSchemaSnapshotTests
/// </summary>
public sealed class CliSchemaSnapshotTests
{
    public static string BuildCanonicalSchema()
    {
        var registry = new CliCommandRegistry();
        registry.Register(new EditorCommands());
        registry.Register(new EntityCommands());
        registry.Register(new MaterialCommands());
        registry.Register(new FrameCaptureCommands());
        registry.Register(new SequenceCommands());
        registry.Register(new RunCommands());
        registry.Register(new TaskCommands());
        registry.Register(new PhysicsCommands());
        registry.Register(new ScriptCommands());
        registry.Register(new IntegrationCommands());
        registry.Register(new PrefabCommands());
        registry.Register(new DeviceDataCommands());
        registry.Register(new PrintCommands());
        registry.Register(new GameplayCommands());

        var options = new JsonSerializerOptions
        {
            WriteIndented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        };
        return JsonSerializer.Serialize(registry.BuildSchema(), options);
    }

    private static string? FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (; dir != null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "Primal.sln")))
                return dir.FullName;
        }
        return null;
    }

    [Fact]
    public void CommandFace_MatchesCommittedSnapshot()
    {
        var root = FindRepoRoot()
            ?? throw new InvalidOperationException("未定位到仓库根（Primal.sln）");
        var snapshotPath = Path.Combine(root, "Testing", "CliSchema", "editor-schema-snapshot.json");

        var actual = BuildCanonicalSchema();

        if (Environment.GetEnvironmentVariable("PRIMAL_UPDATE_SCHEMA") == "1")
        {
            Directory.CreateDirectory(Path.GetDirectoryName(snapshotPath)!);
            File.WriteAllText(snapshotPath, actual + Environment.NewLine);
            return;
        }

        Assert.True(File.Exists(snapshotPath),
            $"缺少 schema 快照：{snapshotPath}。生成：PRIMAL_UPDATE_SCHEMA=1 dotnet test --filter CliSchemaSnapshotTests");

        var expected = File.ReadAllText(snapshotPath).TrimEnd();
        Assert.True(actual == expected,
            "命令面 schema 与快照不一致——接口发生了变化。\n" +
            "这是门禁：请在 PR 描述中列出 verb/参数/描述的具体变化；\n" +
            "若变更是有意的，更新快照并提交：\n" +
            "  PRIMAL_UPDATE_SCHEMA=1 dotnet test --filter CliSchemaSnapshotTests\n" +
            $"当前 $version 见测试输出。");
    }
}
