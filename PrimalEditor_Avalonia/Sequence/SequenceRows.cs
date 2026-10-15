using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace PrimalEditor_Avalonia.Sequence;

/// <summary>
/// v2 运行时包的序列行构建器（sequence S1，2026-10-15）。纯函数——导出器与
/// 单测共用；Player 侧行式解析与这里一一对应（同一契约两端的单一描述）。
///
/// 行格式（实体按名引用——Player 包内无 guid 概念）：
///   seqmeta &lt;fps&gt; &lt;durationFrames&gt;
///   seqprop &lt;entityName&gt; &lt;component&gt; &lt;property&gt; &lt;keyCount&gt;      （标量轨）
///     seqkey &lt;frame&gt; &lt;value&gt; &lt;interp:0=linear|1=constant&gt;
///   seqpos &lt;entityName&gt; &lt;keyCount&gt;                                （变换轨）
///     seqposkey &lt;frame&gt; &lt;px py pz&gt; &lt;rx ry rz&gt; &lt;sx sy sz&gt; &lt;interp&gt;
///
/// CameraCut 轨不导出（Player 相机切换是后续切片；编辑器 sequence.render 不受影响）。
/// </summary>
public static class SequenceRows
{
    /// <summary>构建序列行；doc 为 null 返回空列表。nameOf 解析失败（目标实体
    /// 不在场景）的轨跳过并计入 skipped。</summary>
    public static (IReadOnlyList<string> lines, IReadOnlyList<string> skipped) Build(
        SequenceDocument? doc, Func<Guid, string?> nameOf)
    {
        if (doc == null) return (Array.Empty<string>(), Array.Empty<string>());
        var lines = new List<string>();
        var skipped = new List<string>();

        lines.Add($"seqmeta {doc.Fps} {doc.DurationFrames}");

        foreach (var track in doc.Tracks)
        {
            switch (track)
            {
                case PropertyTrack pt when pt.Keys.Count > 0:
                {
                    var name = nameOf(pt.EntityGuid);
                    if (name == null)
                    {
                        skipped.Add($"property:{pt.Component}.{pt.Property}");
                        continue;
                    }
                    lines.Add($"seqprop \"{Sanitize(name)}\" {pt.Component} {pt.Property} {pt.Keys.Count}");
                    foreach (var k in pt.Keys)
                        lines.Add($"seqkey {k.Frame} {Fmt(k.Value)} {(int)k.Interpolation}");
                    break;
                }
                case EventTrack et when et.Keys.Count > 0:
                {
                    // sequence S2：事件轨（seqevent <count> + 逐键 seqeventkey <frame> "<名>"）
                    lines.Add($"seqevent {et.Keys.Count}");
                    foreach (var k in et.Keys)
                        lines.Add($"seqeventkey {k.Frame} \"{k.Event.Replace("\"", "'")}\"");
                    break;
                }
                case TransformTrack tt when tt.Keys.Count > 0:
                {
                    var name = nameOf(tt.EntityGuid);
                    if (name == null)
                    {
                        skipped.Add("transform");
                        continue;
                    }
                    lines.Add($"seqpos \"{Sanitize(name)}\" {tt.Keys.Count}");
                    foreach (var k in tt.Keys)
                        lines.Add(
                            $"seqposkey {k.Frame} " +
                            $"{Fmt(k.Position[0])} {Fmt(k.Position[1])} {Fmt(k.Position[2])} " +
                            $"{Fmt(k.RotationEuler[0])} {Fmt(k.RotationEuler[1])} {Fmt(k.RotationEuler[2])} " +
                            $"{Fmt(k.Scale[0])} {Fmt(k.Scale[1])} {Fmt(k.Scale[2])} {(int)k.Interpolation}");
                    break;
                }
            }
        }
        return (lines, skipped);
    }

    private static string Sanitize(string name) => name.Replace("\"", "'");

    private static string Fmt(float v) =>
        v.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
}
