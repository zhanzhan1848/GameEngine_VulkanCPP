using System;
using System.IO;
using System.Linq;
using System.Text;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Content;

/// <summary>
/// Exports the live scene as a "Primal Runtime Scene v2" package for the
/// PrimalPlayer host (plan §Run/Stop, 2026-09-03; v2 材质注入 2026-10-09，
/// 原规划留痕②). Line-based format — the player parses it with a
/// dependency-free scanner; keep the two sides in sync
/// (Tools/PrimalPlayer/main.cpp RuntimeScene::Load).
///
///   # primal-runtime-scene v2
///   env <24 floats — UpdateSceneEnvironment layout verbatim>
///   entity
///   name "Name"
///   pos x y z                    / rot x y z / scl x y z
///   mesh "<cooked .mesh path>"
///   builtin <sphere|box|plane|capsule>        (内置几何——player 用 ProceduralMeshCreate 现场生成)
///   tex <slot> "<cooked .texture path>"     (slot 0=albedo 1=normal 2=orm)
///   light type intensity r g b range umbra_deg penumbra_deg enabled
///   camera projection fov_deg near far is_main
///   end
///
/// v2 与 v1 的差异（player 两者都接受，未知键本来就被忽略）：
///   - mesh 不再写源资产/sidecar 路径，写 CookedArtifactResolver 解析后的
///     cooked .mesh（与 SceneRuntimeSynchronizer 同一事实源；引擎
///     ImportMeshAsync 拒绝 .model.json）。
///   - 新增 tex 槽位行：MaterialGuids[0] 经 MaterialWorldTextureResolver
///     解析出的世界纹理三元组（与编辑器视口绑定同一条链），槽位空洞省行。
///   - 新增 builtin 行：BuiltinMesh 内置几何实体（MeshGuid 为空）不落资产，
///     player 端 ProceduralMeshCreate 同参数生成（2026-10-14，release S1）。
///   - 新增 navagent 行（2026-10-15，ai-navigation S1）：导航代理实体声明；
///     player 从静态 collider 几何惰性烘焙导航网格，Lua nav 表驱动移动。
///   - 路径优先项目相对（'/' 分隔，相对项目根）；越出项目根的回退绝对路径。
///     player 按包位置（&lt;project&gt;/.Primal/runtime/）反推项目根解析相对路径。
///   - portable=true：所有被引用文件复制进包目录 _assets/，行内路径改写为
///     包相对（_assets/…）；player 解析顺序 = 包内相对 → 项目根。整个目录
///     可搬移/压缩分发（release S1 可搬移资源包）。
///
/// The editor is the single source of truth for the schema (文档模型优先) —
/// the player never reads .primal/scene JSON or the migration logic.
/// </summary>
public static class RuntimeSceneExporter
{
    public const string FormatHeader = "# primal-runtime-scene v2";

    /// <summary>portable 模式资产落地子目录（包文件旁）。</summary>
    public const string PortableAssetsDirName = "_assets";

    /// <summary>
    /// Export <paramref name="scene"/> to
    /// <c>&lt;projectDir&gt;/.Primal/runtime/active.primalrun</c>.
    /// Returns the written path, or null when the scene/project is missing.
    /// </summary>
    public static string? Export(Scene? scene, string? projectDir) => Export(scene, projectDir, portable: false);

    /// <summary>
    /// portable=true：包写到 <c>runtime/portable/&lt;scene&gt;.primalrun</c>，
    /// 引用的 cooked mesh/纹理/脚本复制进包旁 <c>_assets/</c>，行内路径改写为
    /// 包相对——目录整体可搬移（zip 分发/换机器解压即跑）。复制失败：跳过该
    /// 资产并警告（包仍生成，缺项在 player 侧按各自缺省处理，不静默成功）。
    /// </summary>
    public static string? Export(Scene? scene, string? projectDir, bool portable,
        Sequence.SequenceDocument? sequence = null)
    {
        if (scene == null || string.IsNullOrEmpty(projectDir)) return null;

        var runtimeDir = Path.Combine(projectDir, ".Primal", "runtime");
        string path;
        ExportContext ctx;
        if (portable)
        {
            var packageDir = Path.Combine(runtimeDir, "portable");
            Directory.CreateDirectory(packageDir);
            path = Path.Combine(packageDir, $"{SanitizeFileName(scene.Name)}.primalrun");
            ctx = new ExportContext(portable: true, Path.Combine(packageDir, PortableAssetsDirName));
        }
        else
        {
            Directory.CreateDirectory(runtimeDir);
            path = Path.Combine(runtimeDir, "active.primalrun");
            ctx = new ExportContext(portable: false, assetsDir: null);
        }

        var sb = ctx.Sb;
        sb.AppendLine(FormatHeader);
        sb.AppendLine($"scene \"{scene.Name.Replace("\"", "'")}\"");
        AppendEnvironment(sb, scene);

        var mainCameraGuid = scene.Environment.MainCameraEntityGuid;
        foreach (var entity in Enumerate(scene.Entities))
        {
            sb.AppendLine("entity");
            sb.AppendLine($"name \"{entity.Name.Replace("\"", "'")}\"");
            AppendTransform(sb, entity);
            AppendMesh(sb, ctx, entity);
            AppendTextures(sb, ctx, entity);
            AppendScript(sb, ctx, entity);
            AppendPhysics(sb, entity);
            AppendAudio(sb, ctx, entity);
            AppendNavigation(sb, entity);
            // pfx Player 接通（2026-10-15）：.pfx_emitter JSON 资产行（portable 复制进
            // _assets/）。暴露参数覆盖不随行（名字→Murmur3 哈希在引擎侧）——S1 声明。
            var pfxComponent = entity.GetComponent<Components.ParticleFXComponent>();
            if (pfxComponent != null && pfxComponent.IsEnabled
                && !string.IsNullOrEmpty(pfxComponent.AssetPath))
            {
                var pfxPath = System.IO.Path.IsPathRooted(pfxComponent.AssetPath)
                    ? pfxComponent.AssetPath
                    : System.IO.Path.Combine(projectDir ?? "", pfxComponent.AssetPath);
                if (System.IO.File.Exists(pfxPath))
                    sb.AppendLine($"pfx \"{ToPackagePath(ctx, pfxPath, entity.Name)}\"");
                else
                    PrimalEditor_Avalonia.Common.Logger.Warning(
                        $"[Run] pfx asset missing, entity '{entity.Name}' skipped ({pfxComponent.AssetPath})");
            }
            foreach (var light in entity.Components.OfType<LightComponent>())
            {
                // type/intensity/rgb/range/umbra/penumbra/enabled — the same
                // field order AddEntityLight takes; angles stay degrees, the
                // player converts. (uint)LightType mirrors the synchronizer.
                sb.AppendLine(
                    $"light {(uint)light.LightType} {Fmt(light.Intensity)} {Fmt(light.ColorR)} {Fmt(light.ColorG)} {Fmt(light.ColorB)} " +
                    $"{Fmt(light.Range)} {Fmt(light.UmbraAngle)} {Fmt(light.PenumbraAngle)} {(light.IsEnabled ? 1 : 0)}");
            }
            foreach (var camera in entity.Components.OfType<CameraComponent>())
            {
                var isMain = mainCameraGuid.HasValue && mainCameraGuid.Value == entity.EntityGuid.Value;
                sb.AppendLine(
                    $"camera {(uint)camera.Projection} {Fmt(camera.FieldOfViewDegrees)} {Fmt(camera.NearPlane)} {Fmt(camera.FarPlane)} {(isMain ? 1 : 0)}");
            }
            sb.AppendLine("end");
        }

        // sequence S1（2026-10-15）：序列行（实体按名引用；CameraCut 不导出——
        // Player 相机切换是后续切片）。轨道目标不在场景 → 跳过并记 warning。
        if (sequence != null)
        {
            var (lines, skipped) = Sequence.SequenceRows.Build(sequence, guid =>
                scene?.Entities.FirstOrDefault(e => e.EntityGuid.Value == guid)?.Name);
            foreach (var line in lines) sb.AppendLine(line);
            foreach (var sk in skipped)
                PrimalEditor_Avalonia.Common.Logger.Warning($"[Run] sequence track skipped (target not in scene): {sk}");
        }

        File.WriteAllText(path, sb.ToString());
        PrimalEditor_Avalonia.Common.Logger.Info(
            $"[Run] runtime scene exported: {path}{(portable ? $"（portable，{ctx.CopiedFiles.Count} 个资产复制进 {ctx.AssetsDir}）" : "")}");
        return path;
    }

    /// <summary>包内路径规则：项目相对优先（GetRelativePath，'/' 分隔）；
    /// 越出项目根回退绝对路径并留痕——player 两种都接受。</summary>
    private static string ToPackagePath(ExportContext ctx, string absolutePath, string entityName)
    {
        if (ctx.Portable)
        {
            if (ctx.CopiedFiles.TryGetValue(absolutePath, out var cached)) return cached;
            try
            {
                Directory.CreateDirectory(ctx.AssetsDir!);
                var fileName = SanitizeFileName(Path.GetFileName(absolutePath));
                var target = Path.Combine(ctx.AssetsDir, fileName);
                // 同名不同源：追加全路径短哈希，副本互不覆盖（原件永不改写）。
                if (File.Exists(target))
                {
                    var existing = System.Security.Cryptography.SHA1.HashData(File.ReadAllBytes(target));
                    var incoming = System.Security.Cryptography.SHA1.HashData(File.ReadAllBytes(absolutePath));
                    if (!existing.SequenceEqual(incoming))
                    {
                        var hash = Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(
                            System.Text.Encoding.UTF8.GetBytes(absolutePath)))[..6];
                        var stem = Path.GetFileNameWithoutExtension(fileName);
                        fileName = $"{stem}_{hash}{Path.GetExtension(fileName)}";
                        target = Path.Combine(ctx.AssetsDir, fileName);
                    }
                }
                File.Copy(absolutePath, target, overwrite: true);
                var packageRelative = $"{PortableAssetsDirName}/{fileName}";
                ctx.CopiedFiles[absolutePath] = packageRelative;
                return packageRelative;
            }
            catch (Exception ex)
            {
                PrimalEditor_Avalonia.Common.Logger.Warning(
                    $"[Run] portable copy failed for '{absolutePath}' ({ex.Message}) — entity '{entityName}' 行回退项目相对路径");
            }
        }
        var rel = AssetRegistry.GetRelativePath(absolutePath);
        if (rel is { } r) return r.Value;
        PrimalEditor_Avalonia.Common.Logger.Warning(
            $"[Run] asset outside project root, entity '{entityName}' — absolute path exported: {absolutePath}");
        return absolutePath;
    }

    private static string SanitizeFileName(string name)
    {
        foreach (var c in System.IO.Path.GetInvalidFileNameChars())
            name = name.Replace(c, '_');
        return string.IsNullOrEmpty(name) ? "asset" : name;
    }

    private static void AppendEnvironment(StringBuilder sb, Scene scene)
    {
        var env = scene.Environment;
        // 24 floats in the UpdateSceneEnvironment layout — copied verbatim
        // from SceneEnvironmentViewModel.ApplyToNative so the two push
        // paths can never drift.
        var p = new float[24];
        p[0] = env.AmbientR; p[1] = env.AmbientG; p[2] = env.AmbientB; p[3] = env.AmbientIntensity;
        p[4] = env.FogR; p[5] = env.FogG; p[6] = env.FogB;
        p[7] = env.FogNear; p[8] = env.FogFar; p[9] = env.FogDensity;
        p[10] = env.FogEnabled ? 1f : 0f;
        p[11] = env.SkyR; p[12] = env.SkyG; p[13] = env.SkyB;
        p[14] = env.SkyboxEnabled ? 1f : 0f;
        p[15] = (float)(int)env.ToneMapping;
        p[16] = env.Exposure;
        sb.Append("env");
        foreach (var f in p) sb.Append(' ').Append(Fmt(f));
        sb.AppendLine();
    }

    private static void AppendTransform(StringBuilder sb, GameEntity entity)
    {
        var t = entity.GetTransform();
        float px = 0, py = 0, pz = 0, rx = 0, ry = 0, rz = 0, sx = 1, sy = 1, sz = 1;
        if (t != null)
        {
            px = t.PositionX; py = t.PositionY; pz = t.PositionZ;
            rx = t.RotationX; ry = t.RotationY; rz = t.RotationZ;
            sx = t.ScaleX; sy = t.ScaleY; sz = t.ScaleZ;
        }
        sb.AppendLine($"pos {Fmt(px)} {Fmt(py)} {Fmt(pz)}");
        sb.AppendLine($"rot {Fmt(rx)} {Fmt(ry)} {Fmt(rz)}");
        sb.AppendLine($"scl {Fmt(sx)} {Fmt(sy)} {Fmt(sz)}");
    }

    private static void AppendMesh(StringBuilder sb, ExportContext ctx, GameEntity entity)
    {
        var mesh = entity.GetComponent<MeshRendererComponent>();
        if (mesh == null) return;

        // 内置几何（release S1）：无资产 GUID，player 端 ProceduralMeshCreate
        // 同参数生成（参数表与 SceneRuntimeSynchronizer.EnsureBuiltinGeometry 一致）。
        if (mesh.MeshGuid.Value == Guid.Empty)
        {
            if (mesh.BuiltinMesh == BuiltinMeshKind.None) return;
            sb.AppendLine($"builtin {BuiltinMeshName(mesh.BuiltinMesh)}");
            return;
        }
        var info = AssetRegistry.GetByGuid(mesh.MeshGuid.Value);
        if (info == null || string.IsNullOrEmpty(info.FullPath))
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"[Run] mesh asset not registered, entity '{entity.Name}' (guid={mesh.MeshGuid}) skipped");
            return;
        }
        // 与 SceneRuntimeSynchronizer 同一解析链：cooked .mesh 优先，
        // sidecar 旁同名 .mesh 回退。直接写 sidecar 会被引擎
        // ImportMeshAsync 拒绝（2026-10-09 实测，存量问题③）。
        var cooked = CookedArtifactResolver.ResolveCookedPath(info);
        if (string.IsNullOrEmpty(cooked))
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"[Run] cooked mesh not found for entity '{entity.Name}' (source={info.FullPath}) skipped");
            return;
        }
        sb.AppendLine($"mesh \"{ToPackagePath(ctx, cooked, entity.Name)}\"");
    }

    /// <summary>内置几何包内名（player 侧同名映射到 ProceduralMeshCreate 类型码）。</summary>
    internal static string BuiltinMeshName(BuiltinMeshKind kind) => kind switch
    {
        BuiltinMeshKind.Sphere => "sphere",
        BuiltinMeshKind.Box => "box",
        BuiltinMeshKind.Plane => "plane",
        BuiltinMeshKind.Capsule => "capsule",
        _ => "unknown",
    };

    /// <summary>
    /// physics S1（2026-10-14）：collider/rigidbody 行——字段序与
    /// AddEntityCollider/AddEntityRigidBody 一一对应（PhysicsAPI 同契约；
    /// RigidBody 依赖 Collider，player 侧先挂 collider）。形状 0=Box 1=Sphere
    ///（Sphere 用 half[0] 当半径，同编辑器推送）；模式 0=Static 1=Dynamic
    /// 2=Kinematic。场景含任一 rigidbody → player 开 PhysicsSetRunning。
    /// </summary>
    private static void AppendPhysics(StringBuilder sb, GameEntity entity)
    {
        var collider = entity.GetComponent<ColliderComponent>();
        if (collider != null)
        {
            sb.AppendLine(
                $"collider {(uint)collider.Shape} {Fmt(collider.HalfX)} {Fmt(collider.HalfY)} {Fmt(collider.HalfZ)} " +
                $"0 0 0 {(collider.IsTrigger ? 1 : 0)} {collider.Layer}");
        }
        var body = entity.GetComponent<RigidBodyComponent>();
        if (body != null)
        {
            sb.AppendLine(
                $"rigidbody {(uint)body.Mode} {Fmt(body.Mass)} {Fmt(body.Restitution)} {Fmt(body.Friction)} " +
                $"{Fmt(body.VelocityX)} {Fmt(body.VelocityY)} {Fmt(body.VelocityZ)}");
        }
    }

    private static void AppendTextures(StringBuilder sb, ExportContext ctx, GameEntity entity)
    {
        var mesh = entity.GetComponent<MeshRendererComponent>();
        if (mesh == null || mesh.MaterialGuids.Count == 0) return;

        // 世界渲染器只消费第一个材质槽的 [albedo, normal, orm] 三元组
        // （SceneRuntimeSynchronizer.ResolveWorldTextureIds 同契约）；
        // 多材质槽需要引擎侧 per-submesh 材质支持，待渲染专项协调
        // （总规划 §1.2：必须新增渲染才能完成的功能明确标记待协调）。
        var material = AssetRegistry.GetByGuid(mesh.MaterialGuids[0].Value);
        if (material == null || string.IsNullOrEmpty(material.FullPath))
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"[Run] material asset not registered, entity '{entity.Name}' — textures skipped");
            return;
        }
        var paths = MaterialWorldTextureResolver.Resolve(material.FullPath);
        for (var slot = 0; slot < paths.Length; slot++)
        {
            if (string.IsNullOrWhiteSpace(paths[slot])) continue;
            sb.AppendLine($"tex {slot} \"{ToPackagePath(ctx, paths[slot]!, entity.Name)}\"");
        }
    }

    /// <summary>包内路径规则：项目相对优先（GetRelativePath，'/' 分隔）；
    /// 越出项目根回退绝对路径并留痕——player 两种都接受。</summary>
    private static string ToPackagePath(string absolutePath, string entityName)
    {
        var rel = AssetRegistry.GetRelativePath(absolutePath);
        if (rel is { } r) return r.Value;
        PrimalEditor_Avalonia.Common.Logger.Warning(
            $"[Run] asset outside project root, entity '{entityName}' — absolute path exported: {absolutePath}");
        return absolutePath;
    }

    /// <summary>
    /// scripting S1：script 行写 .lua 资产路径（项目相对优先），param 行写
    /// 字符串 KV。param 键不得含空格（player 行式解析按空格切键）——含空格
    /// 键跳过并警告。
    /// </summary>
    private static void AppendScript(StringBuilder sb, ExportContext ctx, GameEntity entity)
    {
        var script = entity.GetComponent<Components.LuaScriptComponent>();
        if (script == null || script.ScriptAssetGuid.Value == Guid.Empty) return;
        var info = AssetRegistry.GetByGuid(script.ScriptAssetGuid.Value);
        if (info == null || string.IsNullOrEmpty(info.FullPath))
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"[Run] script asset not registered, entity '{entity.Name}' (guid={script.ScriptAssetGuid}) skipped");
            return;
        }
        sb.AppendLine($"script \"{ToPackagePath(ctx, info.FullPath, entity.Name)}\"");
        foreach (var (key, value) in script.Parameters)
        {
            if (string.IsNullOrWhiteSpace(key) || key.Contains(' '))
            {
                PrimalEditor_Avalonia.Common.Logger.Warning(
                    $"[Run] param key '{key}' invalid (empty/contains space), entity '{entity.Name}' skipped");
                continue;
            }
            sb.AppendLine($"param {key} \"{value.Replace("\"", "'")}\"");
        }
    }

    /// <summary>
    /// audio S1（2026-10-14）：`audio "&lt;path&gt;" &lt;volume 0..1&gt; &lt;loop 0|1&gt;
    /// &lt;autoplay 0|1&gt;`——Player 注入后经 WAV 头解析 + 平台播放（macOS
    /// AVAudioPlayer）。非空间声（音量全局）。
    /// </summary>
    private static void AppendAudio(StringBuilder sb, ExportContext ctx, GameEntity entity)
    {
        var audio = entity.GetComponent<AudioSourceComponent>();
        if (audio == null || audio.AudioAssetGuid.Value == Guid.Empty) return;
        var info = AssetRegistry.GetByGuid(audio.AudioAssetGuid.Value);
        if (info == null || string.IsNullOrEmpty(info.FullPath))
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"[Run] audio asset not registered, entity '{entity.Name}' (guid={audio.AudioAssetGuid}) skipped");
            return;
        }
        sb.AppendLine(
            $"audio \"{ToPackagePath(ctx, info.FullPath, entity.Name)}\" " +
            $"{Fmt(audio.Volume)} {(audio.Loop ? 1 : 0)} {(audio.AutoPlay ? 1 : 0)}");
    }

    private static string Fmt(float v) => v.ToString("R", System.Globalization.CultureInfo.InvariantCulture);

    /// <summary>
    /// ai-navigation S1（2026-10-15）：`navagent &lt;radius&gt; &lt;height&gt; &lt;speed&gt;`
    /// ——Player 见到任一 navagent 即从静态 collider 几何惰性烘焙导航网格
    /// （首个 navagent 的 radius/height 作烘焙 profile；S1 单 profile），
    /// 脚本经 Lua nav 表（nav.move_to/stop/state）驱动移动。约束：navagent
    /// 实体不要再挂 dynamic rigidbody（双驱动源冲突）。
    /// </summary>
    private static void AppendNavigation(StringBuilder sb, GameEntity entity)
    {
        var nav = entity.GetComponent<NavAgentComponent>();
        if (nav == null) return;
        if (nav.Radius <= 0f || nav.Height <= 0f)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning(
                $"[Run] navagent radius/height invalid, entity '{entity.Name}' skipped");
            return;
        }
        sb.AppendLine($"navagent {Fmt(nav.Radius)} {Fmt(nav.Height)} {Fmt(nav.Speed)}");
    }

    /// <summary>单次导出状态：普通导出只拼行；portable 导出同时收集要复制进
    /// 包目录的资产（abs → 包相对，去重）。</summary>
    private sealed class ExportContext
    {
        public readonly StringBuilder Sb = new(64 * 1024);
        public readonly bool Portable;
        public readonly string? AssetsDir;
        public readonly System.Collections.Generic.Dictionary<string, string> CopiedFiles = new();

        public ExportContext(bool portable, string? assetsDir)
        {
            Portable = portable;
            AssetsDir = assetsDir;
        }
    }

    private static System.Collections.Generic.IEnumerable<GameEntity> Enumerate(
        System.Collections.Generic.IEnumerable<GameEntity> roots)
    {
        foreach (var entity in roots)
        {
            yield return entity;
            foreach (var child in Enumerate(entity.Children))
                yield return child;
        }
    }
}
