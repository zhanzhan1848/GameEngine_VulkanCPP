using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace PrimalEditor_Avalonia.Integration;

/// <summary>
/// 集成中心共享发现服务（software-integrations-and-skills S1）。
///
/// 语义纪律（子规划"数据与接口约定"）：
///   - detect = 纯静态存在性检查（文件存在 / PATH 命中），无进程、无网络；
///   - 查询无副作用；刷新显式请求（CLI refresh / UI 手动）；
///   - manualPath 是显式选择，检测结果不覆盖它；manual 缺失 → ConfigMissing；
///   - "未扫描到" = NotFound（≠ 不存在）；未扫描 = Unknown；
///   - Skill 清单只读扫描，不执行任何 Skill 代码；目录不存在 = 空清单；
///     外部 harness 不可枚举，S1 不产生 External 记录（状态层表达未知）。
/// 启动页/项目面板/CLI/Agent 共用本服务（UI 接线是后续 UI 切片）。
/// </summary>
public sealed class IntegrationCenter
{
    private readonly List<ISoftwareAdapter> _adapters = new();

    /// <summary>Skill 清单扫描根（user/project 目录提供者；测试可注入）。
    /// 返回 (origin, directory) 序列；目录不存在返回空即可。</summary>
    public Func<IEnumerable<(SkillOrigin origin, string directory)>> SkillRootsProvider { get; set; }
        = DefaultSkillRoots;

    /// <summary>用户级缓存路径（测试可注入）。缺省 ~/.primal/integrations-cache.json。</summary>
    public string CachePath { get; set; } = DefaultCachePath();

    public static string DefaultCachePath()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return Path.Combine(home, ".primal", "integrations-cache.json");
    }

    private static IEnumerable<(SkillOrigin, string)> DefaultSkillRoots()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        yield return (SkillOrigin.User, Path.Combine(home, ".primal", "skills"));
        // 项目级根由打开项目的宿主追加（S1 服务层可注入；无项目时无此来源）
    }

    public IntegrationCenter()
    {
        _adapters.Add(new FfmpegAdapter());
    }

    /// <summary>测试/扩展注入适配器（S1 内置 ffmpeg；新增软件不改核心）。</summary>
    public void RegisterAdapter(ISoftwareAdapter adapter)
    {
        if (_adapters.Any(a => a.Id == adapter.Id))
            throw new InvalidOperationException($"适配器 ID 重复: {adapter.Id}");
        _adapters.Add(adapter);
    }

    /// <summary>
    /// 静态发现（无副作用、不写缓存）。manualOverrides: softwareId → 用户路径。
    /// 返回记录按 SoftwareId 稳定排序（UI/CLI 两入口同序同值——合同 A01）。
    /// </summary>
    public List<SoftwareRecord> DetectAll(IReadOnlyDictionary<string, string>? manualOverrides = null)
    {
        var records = new List<SoftwareRecord>();
        foreach (var adapter in _adapters)
        {
            var record = new SoftwareRecord
            {
                SoftwareId = adapter.Id,
                Category = adapter.Category,
                IsRemote = adapter.IsRemote,
                Candidates = adapter.StaticCandidates().ToList(),
                AdapterVersion = adapter.AdapterVersion,
                DetectedAtUtc = DateTime.UtcNow,
                Version = null,   // S1 不启动进程读版本；未知用 null，不编造
            };
            string? manual = null;
            manualOverrides?.TryGetValue(adapter.Id, out manual);
            record.ManualPath = string.IsNullOrWhiteSpace(manual) ? null : manual;

            if (record.ManualPath != null)
            {
                // 显式选择优先：存在即 Discovered；缺失 = 配置缺失（修复入口=重定位）
                if (EntryExists(record.ManualPath))
                {
                    record.Status = DiscoveryStatus.Discovered;
                    record.ResolvedPath = record.ManualPath;
                    record.Source = "manual";
                }
                else
                {
                    record.Status = DiscoveryStatus.ConfigMissing;
                }
            }
            else if (adapter.IsRemote)
            {
                // 远端服务 S1 不做连通探测：登记为 Unknown（"未验证"而非"缺失"）
                record.Status = DiscoveryStatus.Unknown;
            }
            else
            {
                var hit = adapter.StaticCandidates().FirstOrDefault(EntryExists);
                if (hit != null)
                {
                    record.Status = DiscoveryStatus.Discovered;
                    record.ResolvedPath = hit;
                    record.Source = "candidate";
                }
                else
                {
                    record.Status = DiscoveryStatus.NotFound;
                }
            }
            records.Add(record);
        }
        records.Sort((a, b) => string.CompareOrdinal(a.SoftwareId, b.SoftwareId));
        return records;
    }

    /// <summary>入口存在性：绝对/相对路径看文件；裸命令名查 PATH。</summary>
    private static bool EntryExists(string entry)
    {
        if (string.IsNullOrWhiteSpace(entry)) return false;
        if (entry.Contains('/') || entry.Contains('\\')) return File.Exists(entry);
        var path = Environment.GetEnvironmentVariable("PATH") ?? "";
        return path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)
                   .Any(dir => File.Exists(Path.Combine(dir, entry)));
    }

    /// <summary>
    /// 只读 Skill 清单扫描：根目录下的 *.skill.json（skillId/description/requires）。
    /// 目录缺失 = 该来源空清单；文件损坏跳过并计入 Skipped 计数（不静默成功）。
    /// </summary>
    public (List<SkillRecord> skills, List<string> skippedFiles) ScanSkills()
    {
        var skills = new List<SkillRecord>();
        var skipped = new List<string>();
        foreach (var (origin, directory) in SkillRootsProvider())
        {
            if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory)) continue;
            foreach (var file in Directory.EnumerateFiles(directory, "*.skill.json"))
            {
                try
                {
                    var record = System.Text.Json.JsonSerializer.Deserialize<SkillRecord>(File.ReadAllText(file));
                    if (record == null || string.IsNullOrWhiteSpace(record.SkillId))
                    {
                        skipped.Add(file);
                        continue;
                    }
                    record.Origin = origin;
                    record.Path = file;
                    skills.Add(record);
                }
                catch (System.Text.Json.JsonException)
                {
                    skipped.Add(file);
                }
            }
        }
        skills.Sort((a, b) => string.CompareOrdinal(a.SkillId, b.SkillId));
        return (skills, skipped);
    }

    /// <summary>刷新并落缓存（返回新清单；写失败给类型化错误字符串，不抛）。</summary>
    public (IntegrationInventory inventory, string? error) RefreshAndSave(
        IReadOnlyDictionary<string, string>? manualOverrides = null)
    {
        var (skills, skipped) = ScanSkills();
        var inventory = new IntegrationInventory
        {
            GeneratedAtUtc = DateTime.UtcNow,
            Software = DetectAll(manualOverrides),
            Skills = skills,
        };
        try
        {
            var dir = Path.GetDirectoryName(CachePath);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            File.WriteAllText(CachePath, inventory.Serialize());
        }
        catch (IOException ex)
        {
            return (inventory, $"缓存写入失败（{ex.Message}）——本轮结果已返回，未持久化");
        }
        _lastSkipped = skipped;
        return (inventory, null);
    }

    private List<string> _lastSkipped = new();

    /// <summary>读缓存（无缓存 → null + null error = 全 Unknown，可后台补扫）。</summary>
    public (IntegrationInventory? inventory, string? error) LoadCache()
    {
        return IntegrationInventory.Load(CachePath);
    }

    /// <summary>最近一次 RefreshAndSave 跳过的 Skill 文件（诊断用，勿作状态源）。</summary>
    public IReadOnlyList<string> LastSkippedSkillFiles => _lastSkipped;
}
