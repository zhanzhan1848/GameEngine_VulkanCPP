using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Threading;
using PrimalEditor_Avalonia.ViewModels.PFX;

namespace PrimalEditor_Avalonia.Editors.Particle;

/// <summary>
/// 内嵌 3D 粒子预览控件（Niagara 式 viewport，点显示风格）。
/// 自绘 Render：背景渐变 + 地面网格 + 按快照投影的彩色粒子点。
/// 30Hz InvalidateVisual 常驻刷新；轨道相机自动缓速旋转。
/// 数据来自 VM 的快照 ABI 读取（PfxGetParticleSnapshot）——与 WriteableBitmap
/// 方案不同，本控件走 DrawingContext，在二级窗口中渲染可靠（曲线编辑器同路径）。
/// </summary>
public sealed class ParticlePreviewControl : Control
{
    public static readonly StyledProperty<PfxEmitterEditorViewModel?> ViewModelProperty =
        AvaloniaProperty.Register<ParticlePreviewControl, PfxEmitterEditorViewModel?>(nameof(ViewModel));

    public PfxEmitterEditorViewModel? ViewModel
    {
        get => GetValue(ViewModelProperty);
        set => SetValue(ViewModelProperty, value);
    }

    private DispatcherTimer? _frameTimer;
    private float _yaw;

    private static readonly IBrush BackgroundTopBrush = new SolidColorBrush(Color.FromRgb(0x2A, 0x2A, 0x3E));
    private static readonly IBrush BackgroundBottomBrush = new SolidColorBrush(Color.FromRgb(0x18, 0x18, 0x24));
    private static readonly IBrush GridBrush = new SolidColorBrush(Color.FromRgb(0x46, 0x46, 0x52));
    private static readonly IBrush ParticleBrush = new SolidColorBrush(Colors.White);

    /// <summary>VM 侧单例即可：帧循环在控件 attach 期间驱动 InvalidateVisual。</summary>
    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        if (_frameTimer != null) return;
        _frameTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
        _frameTimer.Tick += (_, _) => InvalidateVisual();
        _frameTimer.Start();
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnDetachedFromVisualTree(e);
        _frameTimer?.Stop();
        _frameTimer = null;
    }

    public ParticlePreviewControl()
    {
        // 粒子模拟在引擎 PipelineTick 中推进；控件只负责显示。
        ClipToBounds = true;
    }

    private struct DrawItem
    {
        public float Sx, Sy, SizePx, Z;
        public byte R, G, B, A;
    }

    public override void Render(DrawingContext ctx)
    {
        float w = (float)Math.Max(Bounds.Width, 1);
        float h = (float)Math.Max(Bounds.Height, 1);

        // 背景纵向渐变
        ctx.FillRectangle(BackgroundTopBrush, new Rect(0, 0, w, h));
        ctx.FillRectangle(BackgroundBottomBrush, new Rect(0, h * 0.55f, w, h * 0.45f));

        var vm = ViewModel;
        if (vm == null) return;

        // 轨道相机（自动缓速旋转；注视原点）
        _yaw += 0.006f;
        const float pitch = 0.42f, dist = 9f;
        float ex = dist * MathF.Cos(_yaw) * MathF.Cos(pitch);
        float ey = dist * MathF.Sin(pitch);
        float ez = dist * MathF.Sin(_yaw) * MathF.Cos(pitch);
        float fl = MathF.Sqrt(ex * ex + ey * ey + ez * ez);
        float fx = -ex / fl, fy = -ey / fl, fz = -ez / fl;
        float rx = -fz, ry2 = 0f, rz = fx;
        float rl = MathF.Sqrt(rx * rx + rz * rz);
        if (rl < 1e-6f) return;
        rx /= rl; rz /= rl;
        float ux = ry2 * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry2 * fx;
        float focal = h * 0.9f;
        float halfW = w * 0.5f, halfH = h * 0.5f;

        (float sx, float sy, float z) Project(float wx, float wy, float wz)
        {
            float dxv = wx - ex, dyv = wy - ey, dzv = wz - ez;
            float z = dxv * fx + dyv * fy + dzv * fz;
            float xc = dxv * rx + dyv * ry2 + dzv * rz;
            float yc = dxv * ux + dyv * uy + dzv * uz;
            return (halfW + (z > 0.05f ? xc * focal / z : 0f),
                    halfH - (z > 0.05f ? yc * focal / z : 0f), z);
        }

        // 地面参考网格 y=0，[-4,4]
        var gridPen = new Pen(GridBrush, 1, dashStyle: DashStyle.Dash, lineCap: PenLineCap.Flat);
        for (float g = -4f; g <= 4.001f; g += 1f)
        {
            var (ax, ay, az) = Project(-4f, 0f, g);
            var (bx, by, bz) = Project(4f, 0f, g);
            if (az > 0.05f && bz > 0.05f)
                ctx.DrawLine(gridPen, new Point(ax, ay), new Point(bx, by));
            var (cx2, cy2, cz2) = Project(g, 0f, -4f);
            var (dx2, dy2, dz2) = Project(g, 0f, 4f);
            if (cz2 > 0.05f && dz2 > 0.05f)
                ctx.DrawLine(gridPen, new Point(cx2, cy2), new Point(dx2, dy2));
        }

        // P1.5:instance 感知视觉——renderer=mesh 时标注实例资产名,粒子画旋转方块
        bool isMesh = vm.IsRenderMesh;
        string? instName = vm.InstanceMeshSelected;
        if (isMesh)
        {
            var label = string.IsNullOrWhiteSpace(instName) ? "INSTANCE: (内置立方体)" : $"INSTANCE: {instName}";
            var ft = new FormattedText(label, System.Globalization.CultureInfo.CurrentCulture,
                FlowDirection.LeftToRight, new Typeface(FontFamily.Default), 11,
                new SolidColorBrush(Color.FromRgb(0x8A, 0xC8, 0xFF)));
            ctx.DrawText(ft, new Point(8, 6));
        }

        // 粒子：读快照 → 投影 → 远到近画圆点
        if (!vm.TryGetPreviewSnapshot(out var posSize, out var color, out var count)) return;

        var drawItems = new DrawItem[count];
        int n = 0;
        for (uint i = 0; i < count; ++i)
        {
            var (sx, sy, z) = Project(posSize[i * 4], posSize[i * 4 + 1], posSize[i * 4 + 2]);
            if (z <= 0.1f || sx < -20 || sy < -20 || sx > w + 20 || sy > h + 20) continue;
            drawItems[n] = new DrawItem
            {
                Sx = sx,
                Sy = sy,
                SizePx = Math.Clamp(MathF.Max(0.05f, posSize[i * 4 + 3]) * focal / z, 1.5f, 40f),
                R = (byte)(Math.Clamp(color[i * 4], 0f, 1f) * 255f),
                G = (byte)(Math.Clamp(color[i * 4 + 1], 0f, 1f) * 255f),
                B = (byte)(Math.Clamp(color[i * 4 + 2], 0f, 1f) * 255f),
                A = (byte)(Math.Clamp(color[i * 4 + 3], 0f, 1f) * 255f),
            };
            drawItems[n].Z = z;
            ++n;
        }
        // 远→近
        System.Array.Sort(drawItems, (a, b) => b.Z.CompareTo(a.Z));
        var instanceOutline = new Pen(new SolidColorBrush(Color.FromRgb(0x30, 0x30, 0x40)), 1);
        for (int i = 0; i < n; ++i)
        {
            var d = drawItems[i];
            float radius = d.SizePx * 0.5f;
            var brush = new SolidColorBrush(Color.FromRgb(d.R, d.G, d.B), d.A / 255.0);
            if (isMesh)
            {
                // mesh 实例:伪随机旋转的方块 + 描边,与 billboard 方点区分
                double angle = (i * 41) % 180;
                var rect = new Rect(d.Sx - radius, d.Sy - radius, radius * 2, radius * 2);
                var m = Matrix.CreateRotation(angle * Math.PI / 180.0);
                m = Matrix.CreateTranslation(d.Sx, d.Sy) * m * Matrix.CreateTranslation(-d.Sx, -d.Sy);
                using (ctx.PushTransform(m))
                {
                    ctx.FillRectangle(brush, rect);
                    ctx.DrawRectangle(instanceOutline, rect);
                }
            }
            else
            {
                ctx.FillRectangle(brush, new Rect(d.Sx - radius, d.Sy - radius, radius * 2, radius * 2));
            }
        }
    }
}
