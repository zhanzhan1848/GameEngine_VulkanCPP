using System;
using System.Collections.Generic;
using System.Linq;

namespace PrimalEditor_Avalonia.Scripting;

/// <summary>属性元数据（scripting S2，2026-10-15）：稳定 ID/类型/单位/所有权。</summary>
public enum PropertyTypeCode { Number, String, Bool, Vec3 }

public sealed class PropertyMetadata
{
    public string StableId { get; init; } = "";       // 稳定 ID（业务身份，非地址）
    public PropertyTypeCode Type { get; init; } = PropertyTypeCode.Number;
    /// <summary>单位（空 = 无量纲）。单位转换经 UnitConversions（未知对拒绝）。</summary>
    public string Unit { get; init; } = "";
    /// <summary>数值范围（Number 类型；null = 不限）。范围外 set 拒绝且不改值。</summary>
    public double? MinValue { get; init; }
    public double? MaxValue { get; init; }
    /// <summary>true = 只读（set 拒绝；引擎反馈类属性）。</summary>
    public bool ReadOnly { get; init; }
    /// <summary>所有权：谁有写权（"user"/"engine"/"script"）——冲突拒绝。</summary>
    public string Owner { get; init; } = "user";
}

/// <summary>属性实例（值 + 元数据引用）。</summary>
public sealed class ObservableProperty
{
    public PropertyMetadata Meta { get; }
    private object _value;
    /// <summary>最后一次成功 set 的值类型；与 Meta.Type 不符 → 拒绝。</summary>
    public object Value => _value;

    public ObservableProperty(PropertyMetadata meta, object initial)
    {
        Meta = meta;
        if (!TypeMatches(meta.Type, initial))
            throw new InvalidOperationException(
                $"初始值类型与元数据不符（{meta.StableId} 需要 {meta.Type}）");
        _value = initial;
    }

    /// <summary>写入（A01 语义：范围外/类型不符/只读/所有权冲突 → false 且不改值）。
    /// owner = 写入方身份；与 Meta.Owner 不符 = 所有权冲突拒绝。</summary>
    public bool TrySet(object value, string owner, out string? error)
    {
        error = null;
        if (Meta.ReadOnly)
        {
            error = $"{Meta.StableId} 是只读属性";
            return false;
        }
        if (!string.Equals(owner, Meta.Owner, StringComparison.Ordinal))
        {
            error = $"{Meta.StableId} 归 {Meta.Owner} 所有，{owner} 无写权";
            return false;
        }
        if (!TypeMatches(Meta.Type, value))
        {
            error = $"{Meta.StableId} 需要 {Meta.Type}，收到 {value?.GetType().Name ?? "null"}";
            return false;
        }
        if (Meta.Type == PropertyTypeCode.Number)
        {
            var n = (double)value;
            if (Meta.MinValue is { } min && n < min)
            {
                error = $"{Meta.StableId} 下限 {min}（收到 {n}）——不改值";
                return false;
            }
            if (Meta.MaxValue is { } max && n > max)
            {
                error = $"{Meta.StableId} 上限 {max}（收到 {n}）——不改值";
                return false;
            }
        }
        _value = value;
        return true;
    }

    private static bool TypeMatches(PropertyTypeCode t, object v) => t switch
    {
        PropertyTypeCode.Number => v is double,
        PropertyTypeCode.String => v is string,
        PropertyTypeCode.Bool => v is bool,
        PropertyTypeCode.Vec3 => v is double[] { Length: 3 },
        _ => false,
    };
}

/// <summary>属性 Undo（前后值快照——失败 set 不入栈）。</summary>
public sealed class PropertySetAction : PrimalEditor_Avalonia.Common.IUndoRedoAction
{
    private readonly ObservableProperty _property;
    private readonly object _before;
    private readonly object _after;
    private readonly string _owner;

    public string Name => $"Set {_property.Meta.StableId}";

    public PropertySetAction(ObservableProperty property, object before, object after, string owner)
    {
        _property = property;
        _before = before;
        _after = after;
        _owner = owner;
    }

    public void Undo() => _property.TrySet(_before, _owner, out _);
    public void Redo() => _property.TrySet(_after, _owner, out _);
}

/// <summary>事件总线（scripting S2）：订阅/解绑；每事件恰投递一次（重载不重订阅）。</summary>
public sealed class ScriptEventBus
{
    public sealed class Subscription { public string EventId; public Action<object?> Handler; }

    private readonly Dictionary<string, List<Action<object?>>> _handlers = new(StringComparer.Ordinal);
    /// <summary>已解绑但避免重投递用的代际（重载安全：Reload++ 后旧 token 失效）。</summary>
    private long _generation;
    public long Generation => _generation;

    public sealed class SubscriptionToken
    {
        public string EventId = "";
        public long Generation;
        internal Action<object?>? Handler;
        internal ScriptEventBus? Bus;
        /// <summary>有效 = 未解绑且代际仍匹配（重载后旧 token 失效）。</summary>
        public bool Active => Handler != null && Bus != null && Bus._generation == Generation;
    }

    /// <summary>订阅（返回 token；同 token 重复订阅 = 无效不叠——防重载重复订阅）。</summary>
    public SubscriptionToken Subscribe(string eventId, Action<object?> handler)
    {
        var token = new SubscriptionToken
        {
            EventId = eventId, Generation = _generation, Handler = handler, Bus = this,
        };
        if (!_handlers.TryGetValue(eventId, out var list))
        {
            list = new List<Action<object?>>();
            _handlers[eventId] = list;
        }
        if (!list.Contains(handler))
            list.Add(handler);
        return token;
    }

    /// <summary>解绑（幂等；代际不匹配的旧 token = 无操作）。</summary>
    public void Unsubscribe(SubscriptionToken token)
    {
        if (token.Generation != _generation || token.Handler == null) return;
        if (_handlers.TryGetValue(token.EventId, out var list))
            list.Remove(token.Handler);
        token.Handler = null;
    }

    /// <summary>热重载：旧 token 全部失效（Generation++），处理器表清空——
    /// 脚本重载后须重新订阅（A01：重载 10 次仍一次投递的根基）。</summary>
    public void OnScriptReload()
    {
        _handlers.Clear();
        ++_generation;
    }

    /// <summary>发布：对当前订阅集恰投递一次/处理器；发布期间的订阅/解绑不影响本轮。</summary>
    public int Publish(string eventId, object? payload)
    {
        if (!_handlers.TryGetValue(eventId, out var list) || list.Count == 0) return 0;
        var snapshot = list.ToList();
        foreach (var h in snapshot) h(payload);
        return snapshot.Count;
    }
}

/// <summary>属性表（脚本组件实例）：元数据注册 + 实例值 + 批量 set/undo 事务。</summary>
public sealed class PropertySet
{
    private readonly Dictionary<string, ObservableProperty> _properties = new(StringComparer.Ordinal);

    public void Define(PropertyMetadata meta, object initial) =>
        _properties[meta.StableId] = new ObservableProperty(meta, initial);

    public ObservableProperty? Find(string stableId) =>
        _properties.TryGetValue(stableId, out var p) ? p : null;

    /// <summary>批量 set（全部成功才生效？否——逐属性独立判定，失败清单返回；
    /// 成功项生成 undo 动作序列由调用方入栈）。A01：范围拒绝且不改值。</summary>
    public (List<PropertySetAction> applied, List<string> rejected) SetMany(
        IEnumerable<(string id, object value)> values, string owner)
    {
        var applied = new List<PropertySetAction>();
        var rejected = new List<string>();
        foreach (var (id, value) in values)
        {
            var prop = Find(id);
            if (prop == null)
            {
                rejected.Add($"{id}: 未注册属性");
                continue;
            }
            var before = prop.Value;
            if (!prop.TrySet(value, owner, out var error))
            {
                rejected.Add(error!);
                continue;
            }
            applied.Add(new PropertySetAction(prop, before, value, owner));
        }
        return (applied, rejected);
    }
}
