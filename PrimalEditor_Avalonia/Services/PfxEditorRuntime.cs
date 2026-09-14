using System;

namespace PrimalEditor_Avalonia.Services;

/// <summary>PFX 运行时状态（编辑器会话级单例）。</summary>
public static class PfxEditorRuntime
{
    /// <summary>当前编辑中的发射器引擎 id（0 = 无）。</summary>
    public static uint CurrentEmitterId { get; set; }
}
