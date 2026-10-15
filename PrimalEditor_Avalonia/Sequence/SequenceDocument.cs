using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Sequence;

/// <summary>
/// Sequence 拍片序列（对标 Unreal Level Sequence，Docs/2026-09-05 Part A）：
/// 一条时间轴上的轨道集合。一期轨道 = TransformTrack（实体位置/旋转/缩放
/// 关键帧，相机运镜 = 相机实体的 Transform 轨）+ CameraCutTrack（分镜头切
/// 相机）。时间完全以帧号为轴（Fps 只决定帧↔秒换算与输出速率），求值无
/// 随机/墙钟依赖——同一序列任意两次求值/渲染逐帧一致（确定性 DoD）。
///
/// 持久化与 Scene/EditorCamera 同惯例：不进 .primal/.scene，独立
/// &lt;project&gt;/Sequences/*.pseq 文件（System.Text.Json；轨道多态用
/// "type" 判别符，同 Component.cs 惯例）。
/// </summary>
[JsonPolymorphic(TypeDiscriminatorPropertyName = "type")]
[JsonDerivedType(typeof(TransformTrack), "transform")]
[JsonDerivedType(typeof(CameraCutTrack), "cameraCut")]
[JsonDerivedType(typeof(PropertyTrack), "property")]
[JsonDerivedType(typeof(EventTrack), "event")]
public abstract class SequenceTrack
{
    /// <summary>轨道在时间轴面板上的显示名（如 "Camera" / 实体名）。</summary>
    public string Name { get; set; } = "Track";
}

/// <summary>实体变换关键帧轨。Keys 按 Frame 升序维护（mutator 保证有序）。</summary>
public sealed class TransformTrack : SequenceTrack
{
    /// <summary>被动画的实体 guid（右手系 +Y 向上、米/度——与 Transform 组件同契约）。</summary>
    public Guid EntityGuid { get; set; }

    public List<TransformKey> Keys { get; set; } = new();
}

public enum KeyInterpolation
{
    /// <summary>与右邻关键帧线性插值（尾帧后 clamp 到末帧值）。</summary>
    Linear,

    /// <summary>阶梯：取区间左关键帧值，直到下一关键帧。</summary>
    Constant,
}

public sealed class TransformKey
{
    /// <summary>关键帧所在帧号（0 起，整数帧；亚帧精度留二期）。</summary>
    public int Frame { get; set; }

    /// <summary>位置 [x,y,z]，米。</summary>
    public float[] Position { get; set; } = new float[3];

    /// <summary>欧拉角 [x,y,z]，度（与 Transform.RotationX/Y/Z 同序）。</summary>
    public float[] RotationEuler { get; set; } = new float[3];

    /// <summary>缩放 [x,y,z]，分量必须 &gt; 0。</summary>
    public float[] Scale { get; set; } = new float[3] { 1f, 1f, 1f };

    [JsonConverter(typeof(JsonStringEnumConverter))]
    public KeyInterpolation Interpolation { get; set; } = KeyInterpolation.Linear;
}

/// <summary>分镜头切相机轨：每个 shot 声明一段帧区间由哪个相机实体取景。
/// 至多一条 CameraCutTrack 参与求值；无 shot 覆盖的帧回落编辑器相机。</summary>
public sealed class CameraCutTrack : SequenceTrack
{
    public List<CameraShot> Shots { get; set; } = new();
}

public sealed class CameraShot
{
    /// <summary>起始帧（含）。建议与相邻 shot 的 EndFrame 相接，不留空隙。</summary>
    public int StartFrame { get; set; }

    /// <summary>结束帧（不含）——半开区间 [StartFrame, EndFrame)，相邻 shot
    /// 不重叠；EndFrame ≤ StartFrame 的 shot 视为无效（求值跳过）。</summary>
    public int EndFrame { get; set; }

    public Guid CameraEntityGuid { get; set; }
}

/// <summary>
/// 通用属性关键帧（sequence S1）：标量属性（如 light.intensity）。
/// 插值语义与 TransformKey 一致——Interpolation 描述"本帧→右邻帧"的过渡。
/// </summary>
public sealed class PropertyKey
{
    /// <summary>关键帧所在帧号（0 起，整数帧）。</summary>
    public int Frame { get; set; }

    /// <summary>属性值（light.intensity：坎德拉量纲同 LightComponent）。</summary>
    public float Value { get; set; }

    [JsonConverter(typeof(JsonStringEnumConverter))]
    public KeyInterpolation Interpolation { get; set; } = KeyInterpolation.Linear;
}

/// <summary>
/// 通用属性轨（sequence S1）：把实体的一个标量属性动画化。S1 支持面 =
/// SupportedProperties 声明的（component, property）对；transform 类动画
/// 走 TransformTrack（九分量轨），属性轨只做标量。Keys 按 Frame 升序维护。
/// </summary>
public sealed class PropertyTrack : SequenceTrack
{
    /// <summary>S1 支持的 (component, property) 白名单——求值/导出/Player
    /// 三端同一事实源；新增属性须同步 Player 行式解析与 EngineDLL setter。</summary>
    public static readonly (string Component, string Property)[] SupportedProperties =
    {
        ("light", "intensity"),
    };

    public static bool IsSupported(string component, string property) =>
        SupportedProperties.Any(p => p.Component == component && p.Property == property);

    /// <summary>被动画的实体 guid。</summary>
    public Guid EntityGuid { get; set; }

    /// <summary>属性宿主组件（"light"）。</summary>
    public string Component { get; set; } = "light";

    /// <summary>属性名（"intensity"）。</summary>
    public string Property { get; set; } = "intensity";

    public List<PropertyKey> Keys { get; set; } = new();
}

/// <summary>事件轨关键帧（sequence S2，2026-10-15）：帧号 + 事件名。</summary>
public sealed class EventKey
{
    /// <summary>触发帧（该帧被播放经过时触发——含）。</summary>
    public int Frame { get; set; }

    /// <summary>事件名（宿主语义——如 "play-sfx"、"open-door"）。</summary>
    public string Event { get; set; } = "";
}

/// <summary>
/// 事件轨：帧轴上的离散事件。求值语义（S2 合同）：事件触发次数精确——
/// 播放推进跨过触发帧时恰一次；seek 跳帧不触发（业务事件不被 scrub 误触）。
/// </summary>
public sealed class EventTrack : SequenceTrack
{
    public List<EventKey> Keys { get; set; } = new();
}

public sealed class SequenceDocument
{
    public const int CurrentVersion = 1;

    public int Version { get; set; } = CurrentVersion;

    public string Name { get; set; } = "Sequence";

    /// <summary>帧率（帧/秒），1..120。只参与帧↔秒换算与视频编码速率，
    /// 不参与求值（时间是帧号轴）。</summary>
    public int Fps { get; set; } = 30;

    /// <summary>序列长度（帧）。有效帧区间 [0, DurationFrames)。</summary>
    public int DurationFrames { get; set; } = 120;

    public List<SequenceTrack> Tracks { get; set; } = new();

    /// <summary>至多一条相机 cut 轨的便捷访问（新建时由调用方保证唯一）。</summary>
    public CameraCutTrack? CameraCut =>
        Tracks.OfType<CameraCutTrack>().FirstOrDefault();

    public TransformTrack? FindTransformTrack(Guid entityGuid) =>
        Tracks.OfType<TransformTrack>().FirstOrDefault(t => t.EntityGuid == entityGuid);

    public EventTrack? FindEventTrack() =>
        Tracks.OfType<EventTrack>().FirstOrDefault();

    public PropertyTrack? FindPropertyTrack(Guid entityGuid, string component, string property) =>
        Tracks.OfType<PropertyTrack>().FirstOrDefault(
            t => t.EntityGuid == entityGuid && t.Component == component && t.Property == property);

    // ------------------------------------------------------------------
    // 持久化（&lt;project&gt;/Sequences/*.pseq）
    // ------------------------------------------------------------------

    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        WriteIndented = true,

        // Load 容忍手编 JSON 的 camelCase 字段（写侧保持 PascalCase 不变）。
        PropertyNameCaseInsensitive = true,
    };

    public static string GetDirectory(string projectPath) =>
        Path.Combine(projectPath, "Sequences");

    public void Save(string path)
    {
        Version = CurrentVersion;
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, JsonSerializer.Serialize(this, SerializerOptions));
    }

    /// <summary>读取 .pseq；文件不存在/JSON 非法/版本不识别时抛
    /// InvalidOperationException（消息含路径与原因，CLI 层转 CliError）。</summary>
    public static SequenceDocument Load(string path)
    {
        if (!File.Exists(path))
            throw new InvalidOperationException($"序列文件不存在: {path}");
        SequenceDocument? doc;
        try
        {
            doc = JsonSerializer.Deserialize<SequenceDocument>(File.ReadAllText(path), SerializerOptions);
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException($"序列文件 JSON 非法: {path} — {ex.Message}");
        }
        if (doc == null)
            throw new InvalidOperationException($"序列文件为空: {path}");
        if (doc.Version > CurrentVersion)
            throw new InvalidOperationException(
                $"序列版本过新: {path} (version {doc.Version} > {CurrentVersion})——请升级编辑器");
        return doc;
    }
}
