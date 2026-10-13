using System.IO;
using PrimalEditor_Avalonia.Cli;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Cli;

/// <summary>
/// agent-workflows S1：长任务登记处契约——提交/完成/失败/取消/产物实算。
/// 会话内存态（静态）：每例结束不清理历史（顺序无耦合，断言只看本例
/// 返回的 id）。
/// </summary>
public sealed class TaskRegistryTests
{
    [Fact]
    public void Submit_AssignsSequentialIdsAndRunningState()
    {
        var a = TaskRegistry.Submit("player-run", "a");
        var b = TaskRegistry.Submit("encode", "b");

        Assert.Equal(TaskRegistry.TaskState.Running, a.State);
        Assert.Equal(TaskRegistry.TaskState.Running, b.State);
        Assert.NotEqual(a.Id, b.Id);
        Assert.Matches(@"^t\d+$", a.Id);
        Assert.True(a.SubmittedAtUtc <= b.SubmittedAtUtc);
        Assert.Equal(a.Id, TaskRegistry.Find(a.Id)!.Id);
        Assert.Null(TaskRegistry.Find("no-such"));
    }

    [Fact]
    public void Complete_MarksSucceededOrFailedWithTimestampAndError()
    {
        var ok = TaskRegistry.Submit("encode", "ok");
        var bad = TaskRegistry.Submit("encode", "bad");

        TaskRegistry.Complete(ok, true);
        TaskRegistry.Complete(bad, false, "ffmpeg missing");

        Assert.Equal(TaskRegistry.TaskState.Succeeded, ok.State);
        Assert.Null(ok.Error);
        Assert.NotNull(ok.FinishedAtUtc);
        Assert.Equal(TaskRegistry.TaskState.Failed, bad.State);
        Assert.Equal("ffmpeg missing", bad.Error);
    }

    [Fact]
    public void TryCancel_InvokesCallbackAndMarksCancelled()
    {
        var cancelled = 0;
        var task = TaskRegistry.Submit("player-run", "c", cancel: () => ++cancelled);

        Assert.True(task.Cancellable);
        Assert.True(TaskRegistry.TryCancel(task.Id, out _));
        Assert.Equal(1, cancelled);
        Assert.Equal(TaskRegistry.TaskState.Cancelled, task.State);
        Assert.False(task.Cancellable);

        // 已结束任务再取消：幂等 false，不再触发回调
        Assert.False(TaskRegistry.TryCancel(task.Id, out var reason));
        Assert.Equal(1, cancelled);
        Assert.Contains("已结束", reason);
    }

    [Fact]
    public void TryCancel_RejectsUncancellableAndUnknown()
    {
        var uncancellable = TaskRegistry.Submit("encode", "one-shot");

        Assert.False(uncancellable.Cancellable);
        Assert.False(TaskRegistry.TryCancel(uncancellable.Id, out var reason1));
        Assert.Contains("不支持", reason1);

        Assert.False(TaskRegistry.TryCancel("t999999", out var reason2));
        Assert.Contains("不存在", reason2);
    }

    [Fact]
    public void DescribeArtifact_ComputesExistenceAndBytesAtQueryTime()
    {
        var path = Path.Combine(Path.GetTempPath(), "PrimalTask_" + Path.GetRandomFileName());
        File.WriteAllBytes(path, new byte[] { 1, 2, 3, 4 });
        try
        {
            dynamic before = TaskRegistry.DescribeArtifact(
                new TaskRegistry.TaskArtifact { Path = path, Kind = "video" })!;
            Assert.True((bool)before.exists);
            Assert.Equal(4L, (long)before.bytes);
            Assert.Equal("video", (string)before.kind);

            File.Delete(path);
            dynamic after = TaskRegistry.DescribeArtifact(
                new TaskRegistry.TaskArtifact { Path = path, Kind = "video" })!;
            Assert.False((bool)after.exists);
            Assert.Equal(0L, (long)after.bytes);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }
}
