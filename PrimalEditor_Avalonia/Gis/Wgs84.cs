using System;

namespace PrimalEditor_Avalonia.Gis;

/// <summary>大地坐标（GIS S1，2026-10-15）。WGS84 经纬度（度）+ 椭球高（米）。</summary>
public readonly record struct GeodeticCoord(double LatitudeDeg, double LongitudeDeg, double HeightMeters);

/// <summary>地心地固坐标（米）。</summary>
public readonly record struct EcefCoord(double X, double Y, double Z);

/// <summary>本地站心坐标（米）：以锚点为原点的 East/North/Up。</summary>
public readonly record struct EnuCoord(double East, double North, double Up);

/// <summary>
/// WGS84 坐标转换（GIS S1）。纯数学层——独立于引擎变换（引擎 Transform 用
/// 米/右手系，本地场景原点 = 锚点；大坐标渲染是渲染专项的待协调项，本模块
/// 只负责把地理坐标正确落到本地 ENU）。
/// 基准：WGS84（a=6378137m，1/f=298.257223563）。
/// 已知值验收（子规划 S1）：参考点往返误差 &lt; 1e-6°，ECEF↔ENU &lt; 1mm。
/// </summary>
public static class Wgs84
{
    public const double SemiMajorAxis = 6378137.0;
    public const double Flattening = 1.0 / 298.257223563;
    public static double SemiMinorAxis => SemiMajorAxis * (1.0 - Flattening);
    public static double FirstEccentricitySq => Flattening * (2.0 - Flattening);

    private static readonly double Deg = Math.PI / 180.0;
    private static readonly double Rad = 180.0 / Math.PI;

    /// <summary>大地坐标 → ECEF（标准 Bowring 无迭代表达式）。</summary>
    public static EcefCoord ToEcef(GeodeticCoord g)
    {
        var lat = g.LatitudeDeg * Deg;
        var lon = g.LongitudeDeg * Deg;
        var sinLat = Math.Sin(lat);
        var cosLat = Math.Cos(lat);
        var sinLon = Math.Sin(lon);
        var cosLon = Math.Cos(lon);
        var n = SemiMajorAxis / Math.Sqrt(1.0 - FirstEccentricitySq * sinLat * sinLat);
        return new EcefCoord(
            (n + g.HeightMeters) * cosLat * cosLon,
            (n + g.HeightMeters) * cosLat * sinLon,
            (n * (1.0 - FirstEccentricitySq) + g.HeightMeters) * sinLat);
    }

    /// <summary>ECEF → 大地坐标（Bowring 迭代法，收敛阈值 1e-12 弧度）。</summary>
    public static GeodeticCoord ToGeodetic(EcefCoord e)
    {
        var lon = Math.Atan2(e.Y, e.X);
        var p = Math.Sqrt(e.X * e.X + e.Y * e.Y);
        var lat = Math.Atan2(e.Z, p * (1.0 - FirstEccentricitySq));
        double sinLat;
        double n;
        // 不动点迭代：3~4 轮收敛到双精度
        for (int i = 0; i < 8; ++i)
        {
            sinLat = Math.Sin(lat);
            n = SemiMajorAxis / Math.Sqrt(1.0 - FirstEccentricitySq * sinLat * sinLat);
            var next = Math.Atan2(e.Z + FirstEccentricitySq * n * sinLat, p);
            if (Math.Abs(next - lat) < 1e-13) { lat = next; break; }
            lat = next;
        }
        sinLat = Math.Sin(lat);
        n = SemiMajorAxis / Math.Sqrt(1.0 - FirstEccentricitySq * sinLat * sinLat);
        var height = p / Math.Cos(lat) - n;
        return new GeodeticCoord(lat * Rad, lon * Rad, height);
    }

    /// <summary>以 anchor 为站心的 ENU 基向量（行优先 3×3 旋转）。</summary>
    public static (double[] East, double[] North, double[] Up) EnuBasis(GeodeticCoord anchor)
    {
        var lat = anchor.LatitudeDeg * Deg;
        var lon = anchor.LongitudeDeg * Deg;
        var sinLat = Math.Sin(lat); var cosLat = Math.Cos(lat);
        var sinLon = Math.Sin(lon); var cosLon = Math.Cos(lon);
        return (
            new[] { -sinLon, cosLon, 0.0 },
            new[] { -sinLat * cosLon, -sinLat * sinLon, cosLat },
            new[] { cosLat * cosLon, cosLat * sinLon, sinLat });
    }

    /// <summary>ECEF → 站心 ENU（米）。</summary>
    public static EnuCoord ToEnu(EcefCoord ecef, GeodeticCoord anchor)
    {
        var origin = ToEcef(anchor);
        var dx = ecef.X - origin.X;
        var dy = ecef.Y - origin.Y;
        var dz = ecef.Z - origin.Z;
        var (e1, n1, u1) = EnuBasis(anchor);
        return new EnuCoord(
            e1[0] * dx + e1[1] * dy + e1[2] * dz,
            n1[0] * dx + n1[1] * dy + n1[2] * dz,
            u1[0] * dx + u1[1] * dy + u1[2] * dz);
    }

    /// <summary>站心 ENU → ECEF。</summary>
    public static EcefCoord FromEnu(EnuCoord enu, GeodeticCoord anchor)
    {
        var origin = ToEcef(anchor);
        var (e1, n1, u1) = EnuBasis(anchor);
        return new EcefCoord(
            origin.X + e1[0] * enu.East + n1[0] * enu.North + u1[0] * enu.Up,
            origin.Y + e1[1] * enu.East + n1[1] * enu.North + u1[1] * enu.Up,
            origin.Z + e1[2] * enu.East + n1[2] * enu.North + u1[2] * enu.Up);
    }

    /// <summary>便捷：大地坐标 → 以 anchor 为原点的本地 ENU（引擎场景落位用）。</summary>
    public static EnuCoord ToLocalEnu(GeodeticCoord g, GeodeticCoord anchor) =>
        ToEnu(ToEcef(g), anchor);

    /// <summary>便捷：本地 ENU → 大地坐标。</summary>
    public static GeodeticCoord FromLocalEnu(EnuCoord enu, GeodeticCoord anchor) =>
        ToGeodetic(FromEnu(enu, anchor));
}
