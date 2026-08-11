using System;
using Avalonia;
using Avalonia.Media;
using PrimalEditor_Avalonia.Editors.Geometry;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// Managed wireframe renderer. Draws the geometry LOD's mesh triangles
/// into an Avalonia DrawingContext. Pure CPU projection — no engine GPU
/// upload required. Sufficient for editor previews up to ~5K vertices;
/// falls behind at 50K+ vertices.
/// </summary>
public static class WireframeRenderer
{
    public readonly struct ViewState
    {
        public readonly float Yaw;       // radians
        public readonly float Pitch;     // radians
        public readonly float Distance;
        public readonly float TargetX;
        public readonly float TargetY;
        public readonly float TargetZ;
        public readonly float ViewportWidth;
        public readonly float ViewportHeight;

        public ViewState(float yaw, float pitch, float distance,
            float tx, float ty, float tz, float vw, float vh)
        {
            Yaw = yaw; Pitch = pitch; Distance = distance;
            TargetX = tx; TargetY = ty; TargetZ = tz;
            ViewportWidth = vw; ViewportHeight = vh;
        }
    }

    public static void Draw(DrawingContext ctx, MeshLOD? lod, ViewState view, float yawHint = 0f, int? submeshIndex = null)
    {
        if (lod == null) return;
        if (lod.Meshes.Count == 0) return;

        // Camera basis: position is target - forward*distance, where
        // forward matches the world-editor camera convention in
        // EngineViewport3D (yaw=0 → -Z). Pitch tilts around camera right.
        float cy = MathF.Cos(view.Yaw);
        float sy = MathF.Sin(view.Yaw);
        float cp = MathF.Cos(view.Pitch);
        float sp = MathF.Sin(view.Pitch);
        float ox = -sy * cp;
        float oy = -sp;
        float oz = -cy * cp;
        float camX = view.TargetX + ox * view.Distance;
        float camY = view.TargetY + oy * view.Distance;
        float camZ = view.TargetZ + oz * view.Distance;
        float[] forward = new[] { ox, oy, oz };
        float[] worldUp = new[] { 0f, 1f, 0f };
        float[] right = Normalize(Cross(forward, worldUp));
        float[] screenUp = Normalize(Cross(right, forward));

        // Reusable line pen
        var edgePen = new Pen(new SolidColorBrush(Color.FromRgb(80, 200, 255), 0.8));
        var fillPen = new Pen(new SolidColorBrush(Color.FromRgb(255, 100, 100), 0.4));
        var w = view.ViewportWidth;
        var h = view.ViewportHeight;
        float focal = MathF.Min(w, h) * 0.5f;

        for (int meshIdx = 0; meshIdx < lod.Meshes.Count; ++meshIdx)
        {
            if (submeshIndex.HasValue && submeshIndex.Value != meshIdx) continue;
            var mesh = lod.Meshes[meshIdx];
            var pos = mesh.GetPositionsAsFloat3();
            var idx = mesh.GetIndicesAsInt();

            // Project all vertices once per mesh; reuse in triangle loops.
            var screen = new (float x, float y, float depth)[pos.Length / 3];
            for (int i = 0; i < pos.Length; i += 3)
            {
                float px = pos[i + 0] - view.TargetX;
                float py = pos[i + 1] - view.TargetY;
                float pz = pos[i + 2] - view.TargetZ;
                // Express vertex in camera basis
                float u = px * right[0] + py * right[1] + pz * right[2];
                float v = px * screenUp[0] + py * screenUp[1] + pz * screenUp[2];
                float wD = px * forward[0] + py * forward[1] + pz * forward[2];
                if (wD < 0.1f) wD = 0.1f; // behind camera: pin to near plane
                float sx = w * 0.5f + (u / wD) * focal;
                float sy2 = h * 0.5f - (v / wD) * focal;
                screen[i / 3] = (sx, sy2, wD);
            }

            for (int i = 0; i + 2 < idx.Length; i += 3)
            {
                int a = idx[i + 0], b = idx[i + 1], c = idx[i + 2];
                var pa = screen[a]; var pb = screen[b]; var pc = screen[c];

                // Backface cull: cross product of edge vectors; the z
                // component of the cross product in screen space determines
                // facing. In Avalonia's coordinate system, screen +y points
                // down, so positive screen-z cross corresponds to backface.
                float ex1 = pb.x - pa.x, ey1 = pb.y - pa.y;
                float ex2 = pc.x - pa.x, ey2 = pc.y - pa.y;
                float screenZ = ex1 * ey2 - ey1 * ex2;
                if (MathF.Abs(screenZ) < 0.01f) continue; // degenerate
                if (screenZ > 0) continue; // backface

                ctx.DrawLine(edgePen, new Point(pa.x, pa.y), new Point(pb.x, pb.y));
                ctx.DrawLine(edgePen, new Point(pb.x, pb.y), new Point(pc.x, pc.y));
                ctx.DrawLine(edgePen, new Point(pc.x, pc.y), new Point(pa.x, pa.y));
            }
        }
    }

    private static float[] Cross(float[] a, float[] b) => new[]
    {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    };

    private static float[] Normalize(float[] v)
    {
        float len = MathF.Sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len < 1e-6f) return new[] { 0f, 1f, 0f };
        return new[] { v[0] / len, v[1] / len, v[2] / len };
    }
}
