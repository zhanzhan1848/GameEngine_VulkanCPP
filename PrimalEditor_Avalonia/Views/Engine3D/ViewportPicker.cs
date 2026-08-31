using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// Ray-AABB picker. Casts a ray through a world-space entity list and
/// returns the nearest hit. O(n) linear scan; intended for editor
/// selection (sub-1000 entities), not a runtime culling structure.
///
/// Each candidate carries an axis-aligned bounding box. For entities
/// without a known mesh AABB, callers may pass a degenerate box
/// (min == max == center) which degenerates to a point pick — visually
/// "small but pickable".
/// </summary>
public static class ViewportPicker
{
    public readonly struct Pick
    {
        public uint EntityId { get; }
        public float Distance { get; }
        public Pick(uint id, float distance) { EntityId = id; Distance = distance; }
    }

    public readonly struct AABB
    {
        public readonly float MinX, MinY, MinZ, MaxX, MaxY, MaxZ;
        public AABB(float minX, float minY, float minZ, float maxX, float maxY, float maxZ)
        {
            MinX = minX; MinY = minY; MinZ = minZ;
            MaxX = maxX; MaxY = maxY; MaxZ = maxZ;
        }
    }

    /// <summary>
    /// Intersect the ray with each entity's AABB. Returns the nearest hit
    /// or null if nothing was hit.
    /// </summary>
    public static Pick? PickNearest(
        IReadOnlyList<(uint id, AABB bounds)> candidates,
        float rayOx, float rayOy, float rayOz,
        float rayDx, float rayDy, float rayDz)
    {
        if (candidates == null || candidates.Count == 0) return null;
        float bestT = float.MaxValue;
        uint bestId = 0;
        for (int i = 0; i < candidates.Count; ++i)
        {
            var c = candidates[i];
            var t = RayAabbT(rayOx, rayOy, rayOz, rayDx, rayDy, rayDz, c.bounds);
            if (t.HasValue && t.Value >= 0f && t.Value < bestT)
            {
                bestT = t.Value;
                bestId = c.id;
            }
        }
        if (bestId == 0) return null;
        return new Pick(bestId, bestT);
    }

    /// <summary>
    /// Slab-based ray-AABB intersection. Returns the entry distance or
    /// null if no hit. The ray's origin is assumed inside the box for
    /// the negative-t case; the caller's PickNearest filters t &gt;= 0
    /// when needed.
    /// </summary>
    public static float? RayAabbT(
        float ox, float oy, float oz,
        float dx, float dy, float dz,
        AABB box)
    {
        // For each axis, compute tmin/tmax at the two planes.
        float t1, t2, tmin = float.NegativeInfinity, tmax = float.PositiveInfinity;
        if (MathF.Abs(dx) < 1e-6f)
        {
            if (ox < box.MinX || ox > box.MaxX) return null;
        }
        else
        {
            t1 = (box.MinX - ox) / dx; t2 = (box.MaxX - ox) / dx;
            if (t1 > t2) (t1, t2) = (t2, t1);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return null;
        }
        if (MathF.Abs(dy) < 1e-6f)
        {
            if (oy < box.MinY || oy > box.MaxY) return null;
        }
        else
        {
            t1 = (box.MinY - oy) / dy; t2 = (box.MaxY - oy) / dy;
            if (t1 > t2) (t1, t2) = (t2, t1);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return null;
        }
        if (MathF.Abs(dz) < 1e-6f)
        {
            if (oz < box.MinZ || oz > box.MaxZ) return null;
        }
        else
        {
            t1 = (box.MinZ - oz) / dz; t2 = (box.MaxZ - oz) / dz;
            if (t1 > t2) (t1, t2) = (t2, t1);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return null;
        }
        return tmin;
    }
}
