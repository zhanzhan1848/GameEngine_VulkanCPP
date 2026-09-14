using System;

namespace PrimalEditor_Avalonia.Components;

public enum RigidBodyMode
{
    Static = 0,
    Dynamic = 1,
    Kinematic = 2
}

/// <summary>
/// 刚体组件（引擎 rigidbody 组件镜像）。依赖同实体的 Collider 组件。
/// 质量：千克；速度：米/秒。
/// </summary>
public sealed class RigidBodyComponent : Component
{
    private float _mass = 1f;
    private float _restitution = 0.1f;
    private float _friction = 0.5f;

    public RigidBodyMode Mode { get; set; } = RigidBodyMode.Dynamic;
    public float Mass { get => _mass; set => _mass = value > 0 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float Restitution { get => _restitution; set => _restitution = value is >= 0 and <= 1 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float Friction { get => _friction; set => _friction = value is >= 0 and <= 1 ? value : throw new ArgumentOutOfRangeException(nameof(value)); }
    public float VelocityX { get; set; }
    public float VelocityY { get; set; }
    public float VelocityZ { get; set; }

    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) =>
        new MSRigidBody(msEntity);
}
