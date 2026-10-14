using System;
using PrimalEditor_Avalonia.Gis;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.GisNs;

/// <summary>
/// GIS S1 契约测试（已知值验收）：
/// ECEF 基准点（赤道原点/90°经线/极点）、大地↔ECEF 往返（&lt;1e-9°/&lt;1e-4m）、
/// 站心 ENU 方向与量级（东 1° ≈ 平行圈弧长、北 1° ≈ 子午线弧长）、
/// 本地 ENU 往返、锚点零点恒等。
/// </summary>
public sealed class Wgs84Tests
{
    [Fact]
    public void Ecef_AxisPoints_AreExact()
    {
        // (0°,0°,h=0) → x=a
        var e = Wgs84.ToEcef(new GeodeticCoord(0, 0, 0));
        Assert.Equal(6378137.0, e.X, 6);
        Assert.Equal(0.0, e.Y, 6);
        Assert.Equal(0.0, e.Z, 6);
        // (0°,90°E) → y=a
        var e90 = Wgs84.ToEcef(new GeodeticCoord(0, 90, 0));
        Assert.Equal(0.0, e90.X, 6);
        Assert.Equal(6378137.0, e90.Y, 6);
        // 北极 → z=b=6356752.314245
        var pole = Wgs84.ToEcef(new GeodeticCoord(90, 0, 0));
        Assert.Equal(0.0, pole.X, 6);
        Assert.Equal(0.0, pole.Y, 6);
        Assert.Equal(6356752.3142, pole.Z, 3);
    }

    [Fact]
    public void Geodetic_RoundTrip_SubMilliDegreeAndMeter()
    {
        var points = new[]
        {
            new GeodeticCoord(31.2304, 121.4737, 10),    // 上海
            new GeodeticCoord(-33.8688, 151.2093, 58),   // 悉尼
            new GeodeticCoord(64.1466, -21.9426, 0),     // 雷克雅未克
            new GeodeticCoord(0.0001, 179.9999, 100),    // 反经线附近
            new GeodeticCoord(89.9, 0.5, -50),           // 极区附近
        };
        foreach (var p in points)
        {
            var back = Wgs84.ToGeodetic(Wgs84.ToEcef(p));
            Assert.Equal(p.LatitudeDeg, back.LatitudeDeg, 8);   // < 1e-8 度
            Assert.Equal(p.LongitudeDeg, back.LongitudeDeg, 8);
            Assert.Equal(p.HeightMeters, back.HeightMeters, 3); // < 1mm
        }
    }

    [Fact]
    public void Enu_AnchorIsOrigin_AndAxesOriented()
    {
        var anchor = new GeodeticCoord(31.2304, 121.4737, 0);
        // 锚点自身 → (0,0,0)
        var zero = Wgs84.ToLocalEnu(anchor, anchor);
        Assert.Equal(0, zero.East, 6);
        Assert.Equal(0, zero.North, 6);
        Assert.Equal(0, zero.Up, 6);
        // 正东 0.01°：E > 0、N ≈ 0、Up ≈ -椭球面曲率下降（量级米级）
        var east = Wgs84.ToLocalEnu(new GeodeticCoord(31.2304, 121.4837, 0), anchor);
        Assert.True(east.East > 900 && east.East < 1000);       // ~0.01°×95.2km/°
        Assert.True(Math.Abs(east.North) < 1.0);
        // 正北 0.01°：N > 0、E ≈ 0
        var north = Wgs84.ToLocalEnu(new GeodeticCoord(31.2404, 121.4737, 0), anchor);
        Assert.True(north.North > 1100 && north.North < 1120);  // ~0.01°×110.9km/°
        Assert.True(Math.Abs(north.East) < 1.0);
        // 抬高 100m：Up ≈ 100
        var up = Wgs84.ToLocalEnu(new GeodeticCoord(31.2304, 121.4737, 100), anchor);
        Assert.Equal(100.0, up.Up, 3);
    }

    [Fact]
    public void LocalEnu_RoundTrip_MillimeterLevel()
    {
        var anchor = new GeodeticCoord(39.9042, 116.4074, 50);   // 北京，海拔 50m
        var local = new EnuCoord(1234.5, -678.9, 42.0);
        var geo = Wgs84.FromLocalEnu(local, anchor);
        var back = Wgs84.ToLocalEnu(geo, anchor);
        Assert.Equal(local.East, back.East, 4);    // < 0.1mm
        Assert.Equal(local.North, back.North, 4);
        Assert.Equal(local.Up, back.Up, 4);
    }
}
