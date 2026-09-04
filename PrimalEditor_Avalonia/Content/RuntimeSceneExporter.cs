using System;
using System.IO;
using System.Linq;
using System.Text;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Content;

/// <summary>
/// Exports the live scene as a "Primal Runtime Scene v1" package for the
/// PrimalPlayer host (plan §Run/Stop, 2026-09-03). Line-based format — the
/// player parses it with a dependency-free scanner; keep the two sides in
/// sync (Tools/PrimalPlayer/main.cpp RuntimeScene::Load).
///
///   # primal-runtime-scene v1
///   env <24 floats — UpdateSceneEnvironment layout verbatim>
///   entity
///   name "Name"
///   pos x y z                    / rot x y z / scl x y z
///   mesh "/absolute/source/asset/path"
///   light type intensity r g b range umbra_deg penumbra_deg enabled
///   camera projection fov_deg near far is_main
///   end
///
/// The editor is the single source of truth for the schema (文档模型优先) —
/// the player never reads .primal/scene JSON or the migration logic.
/// </summary>
public static class RuntimeSceneExporter
{
    public const string FormatHeader = "# primal-runtime-scene v1";

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
        sb.AppendLine($"mesh \"{info.FullPath}\"");
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
