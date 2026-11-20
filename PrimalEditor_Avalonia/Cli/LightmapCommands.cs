using System;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using Primal.CliFramework;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// Lightmap 烘焙命令面（2026-10-19 lightmap 计划，M4）。
///
/// scene.bake_lightmaps：后台 CPU 烘焙当前激活场景（引擎侧收集→BVH 积分→
/// LMAP 落盘→页面加载，运行时逐帧自动点亮匹配实体）。长任务登记 task.*
/// 可查进度/取消占位；进度回调来自引擎 worker 线程，经 TaskRegistry 线程
/// 安全转写。产物 = Content/Bakes/&lt;scene&gt;/lightmap.lmap。
/// </summary>
public class LightmapCommands
{
    private static Scene RequireActiveScene() =>
        EditorServices.ActiveScene
        ?? throw CliError.NotReady("当前没有激活场景", "先 scene.load 或 scene.new");

    private static string ResolveOutputDir(Scene scene, string? requested)
    {
        if (!string.IsNullOrWhiteSpace(requested)) return requested!;
        var worldEditor = EditorServices.WorldEditor
            ?? throw CliError.NotReady("当前没有打开的项目", "先在编辑器中打开项目");
        var project = worldEditor.Project!;
        // 相对项目文件旁的 Content/Bakes/<scene-guid>：重烘焙覆盖，跨场景不串。
        return Path.Combine(Path.GetDirectoryName(project.Path)!, "Content", "Bakes",
            scene.Guid.Value.ToString("N"));
    }

    private static readonly string[] PhaseNames =
        { "collect", "rasterize", "trace", "filter", "dilate", "encode" };

    [CliCommand("scene.bake_lightmaps",
        Description = "烘焙当前激活场景的静态间接光到 lightmap（CPU 路径追踪，后台长任务；" +
                      "task.list/task.status 查进度，产物 lightmap.lmap 自动加载进运行时；" +
                      "output_dir 同步导出 lightmap_page.png 可视化视图：HDR→LDR 为 " +
                      "Reinhard+gamma2.2，per-mesh 矩形白框，便于人工查看）。" +
                      "atlas_size 单位像素（默认 1024）；samples 每纹素采样数（默认 64）；" +
                      "bounces 间接反弹次数（默认 2）；texel_density 纹素/米（默认 16，" +
                      "注意大尺度场景需调低）；denoise 双边滤波开关；output_dir 产物目录" +
                      "（默认 Content/Bakes/<scene-guid>）。per-instance 键为实体稳定键" +
                      "（实体 GUID，跨会话稳定）——增删实体或改实体结构后需重烘焙，" +
                      "旧产物对结构已变的实体会失配。失败语义：任务登记为 failed，" +
                      "task.status 的 error 给出引擎侧摘要。",
        Example = """{"atlas_size":1024,"samples":64,"bounces":2,"texel_density":16}""")]
    public object BakeLightmaps(uint atlas_size = 1024, uint samples = 64, uint bounces = 2,
        double texel_density = 16.0, bool denoise = true, string? output_dir = null)
    {
        var scene = RequireActiveScene();
        var dir = ResolveOutputDir(scene, output_dir);
        Directory.CreateDirectory(dir);
        var lmapPath = Path.Combine(dir, "lightmap.lmap");

        var settings = new NativeEngineBridge.LightmapBakeSettings
        {
            AtlasSize = Math.Max(64, atlas_size),
            SamplesPerTexel = Math.Max(1, samples),
            Bounces = Math.Max(1, bounces),
            TexelDensity = (float)texel_density,
            Denoise = denoise ? 1 : 0,
            DilationPx = 8,
            OutputDir = dir,
        };

        // 后台线程跑阻塞烘焙（LightmapBake 同步契约）；进度回调在引擎
        // worker 线程泵出——只做 TaskRegistry 线程安全转写，不碰 UI。
        var task = TaskRegistry.SubmitFromAnyThread(
            "lightmap-bake",
            $"Lightmap bake: {scene.Name} ({settings.AtlasSize}px, {settings.SamplesPerTexel}spp, " +
            $"{settings.Bounces}bounces)");

        _ = Task.Run(() =>
        {
            string statusJson;
            try
            {
                var cap = 1 << 20;
                var buf = new byte[cap];
                // 进度：phase 2(trace) 占绝对时长——映射到 0.05..0.95。
                void Progress(IntPtr user, uint phase, uint done, uint total)
                {
                    if (phase >= PhaseNames.Length) return;
                    double p = phase switch
                    {
                        0 => 0.02,
                        1 => 0.05,
                        2 => total > 0 ? 0.05 + 0.90 * done / total : 0.5,
                        _ => 0.95 + 0.01 * phase,
                    };
                    TaskRegistry.ReportProgress(task, p,
                        $"{PhaseNames[phase]} {done}/{total}");
                }

                var ok = NativeEngineBridge.LightmapBake(ref settings, Progress, IntPtr.Zero,
                    buf, (uint)cap);
                statusJson = TrimStatusBuffer(buf);
                if (ok == 0)
                {
                    TaskRegistry.Complete(task, false, ExtractError(statusJson));
                    return;
                }
            }
            catch (Exception ex)
            {
                TaskRegistry.Complete(task, false, ex.Message);
                return;
            }

            try
            {
                // 成功：把产物登记为任务 artifact；页面已在引擎侧加载
                //（BakeSceneLightmaps 内部 LoadLightmap），无需二次推送。
                // lightmap_page.png 为烘焙侧同步导出的可视化视图
                //（Reinhard+gamma2.2 + per-mesh 白框），一并登记便于查看。
                TaskRegistry.AddArtifact(task, lmapPath, "lightmap");
                var pagePngPath = Path.Combine(dir, "lightmap_page.png");
                if (File.Exists(pagePngPath))
                    TaskRegistry.AddArtifact(task, pagePngPath, "lightmap-page-png");
                TaskRegistry.Complete(task, true);
                // per-instance 统计（2026-11-05 稳定键计划）：entries 数组由引擎
                // 侧 status JSON 返回（per-mesh 清单），只透传数量不重算、不改
                // 引擎。落在完成时刻的 progress 标签上（MainWindow 的 [Lightmap]
                // 轮询与 TaskRecord.ProgressLabel 可见）。
                var entryCount = CountStatusEntries(statusJson);
                if (entryCount >= 0)
                    TaskRegistry.ReportProgress(task, 1.0,
                        $"done: {entryCount} per-instance entries (stable entity keys)");
                // 场景级持久化：UI 线程写文档模型（ViewModelBase 通知 +
                // IsDirty）；页面本身已在引擎侧加载，场景重载时由
                // WorldEditorViewModel 恢复。
                Avalonia.Threading.Dispatcher.UIThread.Post(() =>
                {
                    var current = EditorServices.ActiveScene;
                    if (current == scene)
                        scene.LightmapPage = lmapPath;
                });
            }
            catch (Exception ex)
            {
                TaskRegistry.Complete(task, false, ex.Message);
            }
        });

        return new
        {
            task = task.Id,
            kind = task.Kind,
            scene = scene.Name,
            output = lmapPath,
            settings = new
            {
                atlas_size = settings.AtlasSize,
                samples = settings.SamplesPerTexel,
                bounces = settings.Bounces,
                texel_density = settings.TexelDensity,
                denoise = settings.Denoise == 1,
                dilation_px = settings.DilationPx,
            },
            note = "task.status 查进度；烘焙完成后运行时自动应用（PRIMAL_LIGHTMAP=0 可整体关闭）",
        };
    }

    private static string TrimStatusBuffer(byte[] buf)
    {
        int len = Array.IndexOf(buf, (byte)0);
        if (len < 0) len = buf.Length;
        return Encoding.UTF8.GetString(buf, 0, len);
    }

    private static string ExtractError(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.TryGetProperty("error", out var e)) return e.GetString() ?? "unknown";
        }
        catch
        {
            // 引擎侧状态非法 JSON——原样返回（截断保护）。
        }
        var raw = json.Length > 300 ? json[..300] : json;
        return string.IsNullOrWhiteSpace(raw) ? "engine returned empty status" : raw;
    }

    /// <summary>
    /// 从引擎 status JSON 里取 per-instance entries 总数；-1 = 无 entries
    /// 数据或 JSON 不可解析（老引擎无此字段时不报数）。
    /// </summary>
    private static int CountStatusEntries(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.TryGetProperty("entries", out var entries) &&
                entries.ValueKind == JsonValueKind.Array)
                return entries.GetArrayLength();
        }
        catch
        {
            // 引擎侧状态非法 JSON——按无数据处理。
        }
        return -1;
    }
}
