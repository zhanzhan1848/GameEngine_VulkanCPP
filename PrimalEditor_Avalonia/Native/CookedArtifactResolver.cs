using System;
using System.IO;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Native;

/// <summary>
/// 源资产 → cooked 产物路径解析（供 SceneRuntimeSynchronizer 与 PFX 发射器编辑器
/// 共用）。规则：.model.json/.model → 同名 .mesh（位于 .Primal/Cooked/&lt;platform&gt;/
/// &lt;configuration&gt;/ 下，保持源相对目录）；产物不存在时可回退源文件本身。
/// </summary>
public static class CookedArtifactResolver
{
    public const string DefaultPlatform = "Editor";
    public const string DefaultConfiguration = "DebugEditor";

    /// <summary>解析资产的 cooked 产物绝对路径。无产物且允许回退时返回源文件路径；
    /// 否则 null。CookedPlatform/Configuration 取构建配置默认值（与
    /// SceneRuntimeSynchronizer 的默认一致）。</summary>
    public static string? ResolveCookedPath(
        AssetInfo info, string platform = DefaultPlatform,
        string configuration = DefaultConfiguration, bool useSourceFallback = true)
    {
        ArgumentNullException.ThrowIfNull(info);
        var relative = AssetRegistry.GetRelativePath(info.FullPath);
        if (relative is not { } rel) return useSourceFallback ? info.FullPath : null;

        var projectRoot = AssetRegistry.ProjectRoot;
        if (string.IsNullOrEmpty(projectRoot)) return useSourceFallback ? info.FullPath : null;

        var artifactRelativePath = rel.Value;
        string? sourceArtifactPath = info.FullPath;
        const string modelSidecarSuffix = ".model.json";
        if (artifactRelativePath.EndsWith(modelSidecarSuffix, StringComparison.OrdinalIgnoreCase))
        {
            artifactRelativePath = artifactRelativePath[..modelSidecarSuffix.Length] + ".mesh";
            sourceArtifactPath = info.FullPath[..^modelSidecarSuffix.Length] + ".mesh";
        }
        else if (Path.GetExtension(artifactRelativePath).Equals(".model", StringComparison.OrdinalIgnoreCase))
        {
            artifactRelativePath = Path.ChangeExtension(artifactRelativePath, ".mesh");
            sourceArtifactPath = Path.ChangeExtension(info.FullPath, ".mesh");
        }

        var cookedAbsolute = Path.Combine(
            projectRoot, ".Primal", "Cooked", platform, configuration,
            artifactRelativePath.Replace('/', Path.DirectorySeparatorChar));

        if (File.Exists(cookedAbsolute)) return cookedAbsolute;
        if (useSourceFallback && sourceArtifactPath is not null && File.Exists(sourceArtifactPath))
            return sourceArtifactPath;
        return null;
    }

    /// <summary>绝对路径 → 尽量转 CWD 相对路径（引擎进程按 CWD 解析资产路径，
    /// 与 fields/.pgrid、ShapeLocation mesh 同约定）；无法相对化时返回原绝对路径。</summary>
    public static string ToCwdRelative(string absolutePath)
    {
        try
        {
            var cwd = Directory.GetCurrentDirectory();
            var rel = Path.GetRelativePath(cwd, absolutePath);
            return rel.StartsWith('.') && (rel.StartsWith("..") || rel == ".") ? absolutePath : rel;
        }
        catch
        {
            return absolutePath;
        }
    }
}
