using System.Linq;
using Primal.CliFramework;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// task 域命令（agent-workflows S1）：长任务登记处的查询/取消面。
/// 提交不经过本域——任务由实际启动异步工作的命令（run.launch 的 player
/// 进程、录制完成后的自动编码等）在启动时登记，本域只做只读查询与取消。
/// </summary>
public sealed class TaskCommands
{
    [CliCommand("task.list",
        Description = "列出本编辑器会话的长任务（player 进程录制/PNG→mp4 编码等）。" +
                      "字段：id(t1…)/kind/description/state(Running|Succeeded|Failed|Cancelled)/" +
                      "submittedAtUtc/finishedAtUtc/artifacts 数。会话内存态——编辑器重启即空；" +
                      "断点恢复/持久化排队是后续阶段。按提交时间正序。",
        Example = "{}")]
    public object List()
    {
        var tasks = TaskRegistry.Snapshot();
        return new
        {
            count = tasks.Count,
            running = tasks.Count(t => t.State == TaskRegistry.TaskState.Running),
            tasks = tasks.Select(t => new
            {
                id = t.Id,
                kind = t.Kind,
                description = t.Description,
                state = t.State.ToString(),
                cancellable = t.Cancellable,
                submittedAtUtc = t.SubmittedAtUtc,
                finishedAtUtc = t.FinishedAtUtc,
                artifacts = t.Artifacts.Count,
            }),
        };
    }

    [CliCommand("task.status",
        Description = "查询单个长任务详情，含产物清单（kind/绝对路径/存在性/字节数——" +
                      "存在性在查询时实算，产物可能已被后续步骤消费）。" +
                      "失败：任务不存在返回 NOT_FOUND。",
        Example = """{"taskId":"t1"}""")]
    public object Status(string taskId)
    {
        var task = TaskRegistry.Find(taskId)
            ?? throw CliError.NotFound($"任务 '{taskId}' 不存在", "先 task.list 查看本会话任务");
        return new
        {
            id = task.Id,
            kind = task.Kind,
            description = task.Description,
            state = task.State.ToString(),
            error = task.Error,
            cancellable = task.Cancellable,
            submittedAtUtc = task.SubmittedAtUtc,
            finishedAtUtc = task.FinishedAtUtc,
            artifacts = task.Artifacts.Select(TaskRegistry.DescribeArtifact),
        };
    }

    [CliCommand("task.cancel",
        Description = "取消运行中的长任务（player 进程 = 终止进程；编码类一次性任务" +
                      "不支持中途取消）。幂等语义：已结束的任务返回 cancelled=false " +
                      "而非错误。失败：任务不存在返回 NOT_FOUND。",
        Example = """{"taskId":"t1"}""")]
    public object Cancel(string taskId)
    {
        var task = TaskRegistry.Find(taskId)
            ?? throw CliError.NotFound($"任务 '{taskId}' 不存在", "先 task.list 查看本会话任务");
        if (!TaskRegistry.TryCancel(taskId, out var reason))
            return new { taskId, cancelled = false, state = task.State.ToString(), reason };
        return new { taskId, cancelled = true, state = "Cancelled" };
    }
}
