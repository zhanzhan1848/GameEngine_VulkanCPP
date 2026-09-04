using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>One row of the A8 source-control panel.</summary>
public sealed class ScmFileEntry : ObservableObject
{
    public string RepoRelativePath { get; }
    public GitService.FileStatus Status { get; }
    public string StatusDisplay { get; }

    private bool _isSelected = true;
    public bool IsSelected { get => _isSelected; set => SetProperty(ref _isSelected, value); }

    public ScmFileEntry(string repoRelativePath, GitService.FileStatus status)
    {
        RepoRelativePath = repoRelativePath;
        Status = status;
        StatusDisplay = status switch
        {
            GitService.FileStatus.Modified => "M",
            GitService.FileStatus.Added => "A",
            GitService.FileStatus.Deleted => "D",
            GitService.FileStatus.Untracked => "?",
            _ => " ",
        };
    }
}

/// <summary>
/// A8 源码控制最小集成：git 状态列表 + 提交面板。项目不在仓库内时面板
/// 显示引导文案；全部 git 调用在后台线程（GitService 内进程级 15s 超时）。
/// 刷新时同步把状态写到 AssetRegistry 的 AssetInfo.GitStatus 供内容浏览器
/// 圆点消费。
/// </summary>
public sealed class SourceControlViewModel : ViewModelBase
{
    private string _projectPath = string.Empty;
    private string _repoRoot = string.Empty;
    private bool _isRepo;
    private string _statusText = string.Empty;
    private string _commitMessage = string.Empty;
    private bool _isBusy;

    public ObservableCollection<ScmFileEntry> Files { get; } = new();

    public bool IsRepo { get => _isRepo; private set => SetProperty(ref _isRepo, value); }
    public string RepoRoot { get => _repoRoot; private set => SetProperty(ref _repoRoot, value); }
    public string StatusText { get => _statusText; private set => SetProperty(ref _statusText, value); }
    public string CommitMessage { get => _commitMessage; set => SetProperty(ref _commitMessage, value); }
    public bool IsBusy { get => _isBusy; private set => SetProperty(ref _isBusy, value); }

    public Common.RelayCommand RefreshCommand { get; }
    public AsyncRelayCommand CommitCommand { get; }

    public SourceControlViewModel()
    {
        RefreshCommand = new Common.RelayCommand(() => _ = RefreshAsync());
        CommitCommand = new AsyncRelayCommand(CommitAsync, () => !IsBusy);
    }

    /// <summary>Bind to the current project and kick an initial refresh.</summary>
    public void SetProject(string? projectPath)
    {
        _projectPath = projectPath ?? string.Empty;
        Files.Clear();
        StatusText = string.Empty;
        _ = RefreshAsync();
    }

    public async Task RefreshAsync()
    {
        if (IsBusy) return;
        IsBusy = true;
        CommitCommand.NotifyCanExecuteChanged();
        try
        {
            var projectPath = _projectPath;
            var (files, root, isRepo) = await Task.Run(() =>
            {
                var repo = GitService.ProbeRepo(projectPath);
                if (!repo.IsRepo) return (new System.Collections.Generic.List<GitService.FileEntry>(), string.Empty, false);
                return (GitService.Status(projectPath), repo.RepoRoot, true);
            });

            IsRepo = isRepo;
            RepoRoot = root;
            Files.Clear();
            foreach (var f in files.OrderBy(f => f.RepoRelativePath, StringComparer.Ordinal))
                Files.Add(new ScmFileEntry(f.RepoRelativePath, f.Status));

            StatusText = isRepo
                ? $"{files.Count} changed file(s)"
                : "Not a git repository — run `git init` in the project folder";

            PushStatusesToAssetRegistry(root, files);
        }
        finally
        {
            IsBusy = false;
            CommitCommand.NotifyCanExecuteChanged();
        }
    }

    private async Task CommitAsync()
    {
        if (IsBusy || !IsRepo) return;
        var selected = Files.Where(f => f.IsSelected).Select(f => f.RepoRelativePath).ToList();
        if (selected.Count == 0)
        {
            StatusText = "no files selected";
            return;
        }
        IsBusy = true;
        CommitCommand.NotifyCanExecuteChanged();
        try
        {
            var projectPath = _projectPath;
            var message = CommitMessage;
            var (ok, feedback) = await Task.Run(() =>
                GitService.Commit(projectPath, selected, message));
            StatusText = feedback;
            if (ok)
            {
                CommitMessage = string.Empty;
                await RefreshAsync();
            }
        }
        finally
        {
            IsBusy = false;
            CommitCommand.NotifyCanExecuteChanged();
        }
    }

    /// <summary>Map repo-relative paths onto registered assets so content
    /// browser tiles can show status dots. Outside-repo files simply don't
    /// match an asset and are skipped.</summary>
    private static void PushStatusesToAssetRegistry(
        string repoRoot, List<GitService.FileEntry> files)
    {
        foreach (var asset in AssetRegistry.Assets)
        {
            var status = GitService.FileStatus.None;
            if (!string.IsNullOrEmpty(repoRoot))
            {
                var relative = MakeRepoRelative(asset.FullPath, repoRoot);
                if (relative != null)
                {
                    var hit = files.FirstOrDefault(f => f.RepoRelativePath == relative);
                    status = hit?.Status ?? GitService.FileStatus.None;
                }
            }
            asset.SetGitStatus(status);
        }
    }

    private static string? MakeRepoRelative(string fullPath, string repoRoot)
    {
        if (string.IsNullOrEmpty(fullPath) || string.IsNullOrEmpty(repoRoot)) return null;
        var full = Path.GetFullPath(fullPath);
        var root = Path.GetFullPath(repoRoot);
        var rootWithSep = root.EndsWith(Path.DirectorySeparatorChar)
            ? root : root + Path.DirectorySeparatorChar;
        return full.StartsWith(rootWithSep, StringComparison.OrdinalIgnoreCase)
            ? full[rootWithSep.Length..].Replace('\\', '/')
            : null;
    }
}
