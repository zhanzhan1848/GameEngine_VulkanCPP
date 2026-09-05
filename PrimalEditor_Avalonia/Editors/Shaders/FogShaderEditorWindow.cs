using System;
using System.IO;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Shaders;

/// <summary>
/// B2 增量: 雾着色器编辑器（非模态单例）。直接编辑 Froxel 雾的 MSL 源
/// （FroxelDensityInject / FroxelLightIntegrate / FroxelResolve /
/// FroxelCommon），保存写盘后触发 SetRenderPassEnabled(FroxelFog) off→on
/// 重建——FroxelFogPass::CreatePipelines 会从盘上重读 MSL 完成热更。
/// </summary>
public sealed class FogShaderEditorWindow : Window
{
    private static FogShaderEditorWindow? _instance;

    /// <summary>可编辑的雾着色器文件（Volume 子目录，include 走 Volume 根）。</summary>
    private static readonly string[] ShaderFiles =
    {
        "FroxelDensityInject.metal",
        "FroxelLightIntegrate.metal",
        "FroxelResolve.metal",
        "FroxelCommon.metal",
    };

    private readonly ComboBox _fileCombo = new() { MinWidth = 220 };
    private readonly TextBox _editor = new()
    {
        AcceptsReturn = true,
        TextWrapping = TextWrapping.NoWrap,
        FontFamily = new FontFamily("Menlo,Consolas,monospace"),
        FontSize = 12,
        MinHeight = 380,
    };
    private readonly TextBlock _status = new()
    {
        FontFamily = new FontFamily("Menlo,Consolas,monospace"),
        FontSize = 11,
    };

    private string _currentPath = string.Empty;

    public static void ShowFor(Avalonia.Controls.Window? owner)
    {
        if (_instance == null)
        {
            _instance = new FogShaderEditorWindow();
            _instance.Closed += (_, _) => _instance = null;
        }
        if (owner != null && _instance.Owner == null)
            _instance.Show(owner);
        else
            _instance.Show();
        _instance.Activate();
    }

    public FogShaderEditorWindow()
    {
        Title = "Fog Shader Editor";
        Width = 860;
        Height = 620;

        foreach (var f in ShaderFiles)
            _fileCombo.Items.Add(f);
        _fileCombo.SelectionChanged += (_, _) => LoadSelected();

        var top = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        top.Children.Add(_fileCombo);
        var reload = new Button { Content = "Reload from disk" };
        reload.Click += (_, _) => LoadSelected();
        top.Children.Add(reload);

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto,Auto"), Margin = new Thickness(10) };
        Grid.SetRow(top, 0);
        root.Children.Add(top);

        Grid.SetRow(_editor, 1);
        _editor.Margin = new Thickness(0, 8, 0, 0);
        root.Children.Add(_editor);

        Grid.SetRow(_status, 2);
        _status.Margin = new Thickness(0, 6, 0, 0);
        root.Children.Add(_status);

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 8, 0, 0) };
        var save = new Button { Content = "Save + Hot Reload", Classes = { "accent" } };
        save.Click += (_, _) => SaveAndHotReload();
        buttons.Children.Add(save);
        Grid.SetRow(buttons, 3);
        root.Children.Add(buttons);

        Content = root;

        _fileCombo.SelectedIndex = 0;
    }

    /// <summary>定位 Volume 着色器目录：cwd 相对 → 可执行位置上溯 → 仓库绝对兜底
    ///（与引擎 LoadShaderBytecode 的双候选风格一致）。</summary>
    private static string? ResolveVolumeDir()
    {
        var candidates = new System.Collections.Generic.List<string>
        {
            "Engine/Graphics/Metal/shaders/Volume",
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/Volume",
        };
        try
        {
            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            for (; dir != null; dir = dir.Parent)
            {
                candidates.Add(Path.Combine(dir.FullName, "Engine/Graphics/Metal/shaders/Volume"));
            }
        }
        catch { /* best effort */ }

        foreach (var c in candidates)
        {
            if (File.Exists(Path.Combine(c, "FroxelResolve.metal"))) return c;
        }
        return null;
    }

    private void LoadSelected()
    {
        if (_fileCombo.SelectedItem is not string fileName) return;
        var dir = ResolveVolumeDir();
        if (dir == null)
        {
            _status.Text = "Volume shader directory not found";
            return;
        }
        _currentPath = Path.Combine(dir, fileName);
        try
        {
            _editor.Text = File.ReadAllText(_currentPath);
            _status.Text = $"Loaded {_currentPath}";
            Title = $"Fog Shader Editor — {fileName}";
        }
        catch (Exception ex)
        {
            _status.Text = $"Load failed: {ex.Message}";
        }
    }

    private void SaveAndHotReload()
    {
        if (string.IsNullOrEmpty(_currentPath))
        {
            _status.Text = "No file loaded";
            return;
        }
        try
        {
            File.WriteAllText(_currentPath, _editor.Text ?? string.Empty);
        }
        catch (Exception ex)
        {
            _status.Text = $"Save failed: {ex.Message}";
            return;
        }

        // 热更 = FroxelFog off→on 重建（CreatePipelines 从盘上重读 MSL）。
        // 雾未开启时只写盘：下次开启时生效。
        if (NativeEngineBridge.IsRenderPassEnabled(NativeEngineBridge.RenderPassId.FroxelFog))
        {
            NativeEngineBridge.SetRenderPassEnabled(NativeEngineBridge.RenderPassId.FroxelFog, false);
            NativeEngineBridge.SetRenderPassEnabled(NativeEngineBridge.RenderPassId.FroxelFog, true);
            _status.Text = $"Saved + hot reloaded ({Path.GetFileName(_currentPath)})";
            PrimalEditor_Avalonia.Common.Logger.Info(
                $"[Fog] shader hot reloaded: {Path.GetFileName(_currentPath)}");
        }
        else
        {
            _status.Text = "Saved (fog disabled — will compile when enabled)";
            PrimalEditor_Avalonia.Common.Logger.Info(
                "[Fog] shader saved while fog disabled; compiles on next enable");
        }
    }
}
