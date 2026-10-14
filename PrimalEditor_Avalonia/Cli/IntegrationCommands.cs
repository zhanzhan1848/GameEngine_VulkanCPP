using System.Collections.Generic;
using System.Linq;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Integration;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// 集成中心 verbs（software-integrations-and-skills S1，2026-10-15）。
/// 与启动页/项目面板共用 IntegrationCenter——UI/CLI 同服务同结果（合同 A01）。
/// S1 语义：静态发现（无进程/无网络）；status 含义见 DiscoveryStatus；
/// "NotFound" = 未扫描到（不等于软件不存在）。
/// </summary>
public sealed class IntegrationCommands
{
    private readonly IntegrationCenter _center;

    /// <summary>CliHost 用缺省中心；测试注入临时缓存路径的实例。</summary>
    public IntegrationCommands(IntegrationCenter? center = null)
    {
        _center = center ?? new IntegrationCenter();
    }

    private static string StatusName(DiscoveryStatus s) => s switch
    {
        DiscoveryStatus.Unknown => "unknown",
        DiscoveryStatus.NotFound => "not-found",
        DiscoveryStatus.Discovered => "discovered",
        DiscoveryStatus.ConfigMissing => "config-missing",
        DiscoveryStatus.ProbeFailed => "probe-failed",
        DiscoveryStatus.Verified => "verified",
        _ => "unknown",
    };

    [CliCommand("integrations.list",
        Description = "集成中心：列出软件/Skill 清单（software-integrations S1）。" +
                      "refresh=false 读用户级缓存（无缓存返回 unknown 状态并提示刷新）；" +
                      "refresh=true 重新静态发现并写缓存（无进程启动/无网络访问）。" +
                      "每条软件记录: softwareId/category/isRemote/status/version(null=未知)/" +
                      "resolvedPath/manualPath/candidates。status: unknown=未扫描、" +
                      "not-found=候选与手动路径均未命中（≠软件不存在）、discovered=入口存在" +
                      "（可调用性未验证，probe 属后续阶段）、config-missing=手动路径缺失。" +
                      "Skill 清单只读扫描 *.skill.json（user=…/.primal/skills；项目根 Skills 由宿主注入），" +
                      "损坏文件计入 skipped。纯读 + 显式刷新，无副作用。",
        Example = """{"refresh":true}""")]
    public object List(bool refresh = false)
    {
        IntegrationInventory? inventory;
        string? error;
        bool fromCache = !refresh;
        if (refresh)
        {
            (inventory, error) = _center.RefreshAndSave();
        }
        else
        {
            (inventory, error) = _center.LoadCache();
        }

        if (error != null && inventory == null)
            throw new CliError(CliErrorCodes.InvalidParams, "集成缓存读取失败: " + error,
                "用 {\"refresh\":true} 重建缓存");
        if (inventory == null)
        {
            // 无缓存：返回全 unknown 骨架 + 提示（不静默当作空集成）
            var skeleton = _center.DetectAll().Select(r => new
            {
                r.SoftwareId,
                r.Category,
                r.IsRemote,
                status = "unknown",
                version = (string?)null,
                resolvedPath = (string?)null,
                manualPath = (string?)null,
            }).ToList();
            return new
            {
                source = "empty",
                hint = "尚无缓存——用 {\"refresh\":true} 执行首次静态发现",
                software = skeleton,
                skills = new object[0],
            };
        }

        var software = inventory.Software.Select(r => new
        {
            r.SoftwareId,
            r.Category,
            r.IsRemote,
            status = StatusName(r.Status),
            version = r.Version,
            resolvedPath = r.ResolvedPath,
            manualPath = r.ManualPath,
            candidates = r.Candidates,
            detectedAtUtc = r.DetectedAtUtc,
        }).ToList();
        var skills = inventory.Skills.Select(s => new
        {
            s.SkillId,
            s.Description,
            origin = s.Origin.ToString().ToLowerInvariant(),
            s.Path,
            requires = s.Requires,
            lastValidation = s.LastValidation,
        }).ToList();
        var counts = new
        {
            discovered = software.Count(s => s.status == "discovered"),
            notFound = software.Count(s => s.status == "not-found"),
            unknown = software.Count(s => s.status is "unknown" or "config-missing"),
            skills = skills.Count,
        };
        return new
        {
            source = fromCache ? "cache" : "refresh",
            generatedAtUtc = inventory.GeneratedAtUtc,
            counts,
            software,
            skills,
            skippedSkillFiles = _center.LastSkippedSkillFiles,
            warning = error,   // 刷新成功但缓存写失败等非致命问题
        };
    }
}
