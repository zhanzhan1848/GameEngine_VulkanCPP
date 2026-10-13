using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// agent-workflows S1（2026-10-14）：编辑器内长任务登记处。
/// 长任务 = 超出单条命令同步返回语义的工作（player 进程录制、PNG→mp4 编码、
/// 外部软件流水线步骤）。命令面（task.*）只读写本登记处——提交方在启动
/// 异步工作时登记，完成/失败/取消时回填；产物 = 跨软件可交接的文件路径。
///
/// 会话内存态（S1）：编辑器重启即空。断点恢复/持久化排队是 S2——本层先把
/// "谁在跑、产物在哪、能不能停"变成可查询、可取消的单一事实源。
/// 命令处理器在 UI 线程执行（CliHost marshaler 契约），与提交方（RuntimeLauncher
/// 的完成回调同样 marshal 到 UI 线程）同线程——无锁；跨线程提交用
/// <see cref="SubmitFromAnyThread"/>（内部 lock）。
/// </summary>
public static class TaskRegistry
{
    public enum TaskState { Running, Succeeded, Failed, Cancelled }

    public sealed class TaskArtifact
    {
        public required string Path { get; init; }
        /// <summary>产物角色（capture / video / manifest …），跨软件交接时的语义标注。</summary>
        public required string Kind { get; init; }
    }

    public sealed class TaskRecord
    {
        public required string Id { get; init; }
        public required string Kind { get; init; }
        public required string Description { get; init; }
        public required DateTime SubmittedAtUtc { get; init; }
        public TaskState State { get; internal set; } = TaskState.Running;
        public DateTime? FinishedAtUtc { get; internal set; }
        public string? Error { get; internal set; }
        public List<TaskArtifact> Artifacts { get; } = new();
        internal Action? CancelCallback { get; set; }
        public bool Cancellable => State == TaskState.Running && CancelCallback != null;
    }

    private static readonly object Gate = new();
    private static readonly List<TaskRecord> Tasks = new();
    private static int _nextId;

    /// <summary>UI 线程提交（常规路径）。cancel 为 null = 该任务不可中途取消。</summary>
    public static TaskRecord Submit(string kind, string description,
        Action? cancel = null, IEnumerable<TaskArtifact>? artifacts = null)
    {
        lock (Gate)
        {
            var record = new TaskRecord
            {
                Id = $"t{++_nextId}",
                Kind = kind,
                Description = description,
                SubmittedAtUtc = DateTime.UtcNow,
                CancelCallback = cancel,
            };
            if (artifacts != null) record.Artifacts.AddRange(artifacts);
            Tasks.Add(record);
            return record;
        }
    }

    /// <summary>任意线程提交（进程退出回调等）——与 UI 线程提交互斥合并。</summary>
    public static TaskRecord SubmitFromAnyThread(string kind, string description,
        Action? cancel = null, IEnumerable<TaskArtifact>? artifacts = null)
        => Submit(kind, description, cancel, artifacts);

    public static TaskRecord? Find(string id)
    {
        lock (Gate) return Tasks.FirstOrDefault(t => t.Id == id);
    }

    public static IReadOnlyList<TaskRecord> Snapshot()
    {
        lock (Gate) return Tasks.ToArray();
    }

    public static void Complete(TaskRecord task, bool success, string? error = null)
    {
        lock (Gate)
        {
            task.State = success ? TaskState.Succeeded : TaskState.Failed;
            task.Error = error;
            task.FinishedAtUtc = DateTime.UtcNow;
        }
    }

    /// <summary>取消运行中的任务。返回 false = 任务不存在/已结束/不可取消。</summary>
    public static bool TryCancel(string id, out string? reason)
    {
        lock (Gate)
        {
            var task = Tasks.FirstOrDefault(t => t.Id == id);
            if (task == null) { reason = "任务不存在"; return false; }
            if (task.State != TaskState.Running) { reason = $"任务已结束（{task.State}）"; return false; }
            if (task.CancelCallback == null) { reason = "该任务不支持中途取消"; return false; }
            task.CancelCallback();
            task.State = TaskState.Cancelled;
            task.FinishedAtUtc = DateTime.UtcNow;
            reason = null;
            return true;
        }
    }

    /// <summary>产物状态视图（存在性/字节数在查询时实算——文件可能被外部步骤消费）。</summary>
    public static object DescribeArtifact(TaskArtifact a) => new
    {
        kind = a.Kind,
        path = a.Path,
        exists = File.Exists(a.Path),
        bytes = File.Exists(a.Path) ? new FileInfo(a.Path).Length : 0,
    };
}
