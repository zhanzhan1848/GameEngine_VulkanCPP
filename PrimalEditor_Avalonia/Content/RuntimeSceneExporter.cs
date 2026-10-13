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
///   - 路径优先项目相对（'/' 分隔，相对项目根）；越出项目根的回退绝对路径。
///     player 按包位置（&lt;project&gt;/.Primal/runtime/）反推项目根解析相对路径。
///
/// The editor is the single source of truth for the schema (文档模型优先) —
/// the player never reads .primal/scene JSON or the migration logic.
/// </summary>
public static class RuntimeSceneExporter
{
    public const string FormatHeader = "# primal-runtime-scene v2";

    /// <summary>
    /// Export <paramref name="scene"/> to
    /// <c>&lt;projectDir&gt;/.Primal/runtime/active.primalrun</c>.
    /// Returns the written path, or null when the scene/project is missing.
    /// </summary>
    public static string? Export(Scene? scene, string? projectDir)
    {
        if (scene == null || string.IsNullOrEmpty(projectDir)) return null;

        var sb = new StringBuilder(64 * 1024);
        sb.AppendLine(FormatHeader);
        sb.AppendLine($"scene \"{scene.Name.Replace("\"", "'")}\"");
        AppendEnvironment(sb, scene);

        var mainCameraGuid = scene.Environment.MainCameraEntityGuid;
        foreach (var entity in Enumerate(scene.Entities))
        {
            sb.AppendLine("entity");
            sb.AppendLine($"name \"{entity.Name.Replace("\"", "'")}\"");
            AppendTransform(sb, entity);
            AppendMesh(sb, entity);
            AppendTextures(sb, entity);
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

        var runtimeDir = Path.Combine(projectDir, ".Primal", "runtime");
        Directory.CreateDirectory(runtimeDir);
        var path = Path.Combine(runtimeDir, "active.primalrun");
        File.WriteAllText(path, sb.ToString());
        PrimalEditor_Avalonia.Common.Logger.Info(
            $"[Run] runtime scene exported: {path}");
        return path;
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

    private static void AppendMesh(StringBuilder sb, GameEntity entity)
    {
        var mesh = entity.GetComponent<MeshRendererComponent>();
        if (mesh == null || mesh.MeshGuid.Value == Guid.Empty) return;
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
        sb.AppendLine($"mesh \"{ToPackagePath(cooked, entity.Name)}\"");
    }

    private static void AppendTextures(StringBuilder sb, GameEntity entity)
    {
        var mesh = entity.GetComponent<MeshRendererComponent>();
        if (mesh == null || mesh.MaterialGuids.Count == 0) return;

        // 世界渲染器只消费第一个材质槽的 [albedo, normal, orm] 三元组
        // （SceneRuntimeSynchronizer.ResolveWorldTextureIds 同契约）；
        // 解析器与编辑器视口共用，避免第二份材质图遍历实现。
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
            sb.AppendLine($"tex {slot} \"{ToPackagePath(paths[slot]!, entity.Name)}\"");
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

    private static string Fmt(float v) => v.ToString("R", System.Globalization.CultureInfo.InvariantCulture);

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
