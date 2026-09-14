using System;

namespace PrimalEditor_Avalonia.Components;

public enum ColliderShape
{
    Box = 0,
    Sphere = 1
}

/// <summary>
/// 碰撞体组件（引擎 collider 组件镜像）。无 RigidBody 的 Collider = 静态体。
/// 单位：米。半尺寸 box / 半径 sphere。
/// </summary>
public sealed class ColliderComponent : Component
{
    private float _halfX = 0.5f, _halfY = 0.5f, _halfZ = 0.5f;

    public ColliderShape Shape { get; set; } = ColliderShape.Box;
    public float HalfX { get => _halfX; set => _halfX = value > 0 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float HalfY { get => _halfY; set => _halfY = value > 0 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float HalfZ { get => _halfZ; set => _halfZ = value > 0 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public bool IsTrigger { get; set; }
    public int Layer { get; set; } = 1;

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSCollider(msEntity);
}
