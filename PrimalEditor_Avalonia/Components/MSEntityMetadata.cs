using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// Multi-selection view over <see cref="EntityMetadata"/>. Unlike
/// <see cref="MSTransform"/> / <see cref="MSLight"/> / etc., this
/// component is optional — most entities don't carry it. So instead of
/// filtering out entities without metadata (the standard
/// <see cref="MSComponent{T}"/> behavior), we walk every selected
/// entity, treat absent metadata as the "empty" sentinel, and lazily
/// attach an <see cref="EntityMetadata"/> when the user edits a value
/// or toggles <see cref="EntityMetadata.IsPlayer"/>.
/// </summary>
public sealed class MSEntityMetadata : ViewModelBase, IMSComponent
{
    private readonly MSEntity _msEntity;

    private bool? _isPlayer;
    private string? _description;
    private System.Collections.Generic.List<string>? _sharedTags;
    private bool _hasSharedTagsValue;

    public bool? IsPlayer { get => _isPlayer; set { if (_isPlayer != value) { _isPlayer = value; OnPropertyChanged(); ApplyIsPlayer(); } } }
    public string? Description { get => _description; set { if (_description != value) { _description = value; OnPropertyChanged(); ApplyDescription(); } } }

    /// <summary>Tags shared by all selected entities, or null when values differ.</summary>
    public System.Collections.Generic.IReadOnlyList<string>? SharedTags
        => _hasSharedTagsValue ? _sharedTags : null;

    public string? NewTag { get; set; }
    public System.Windows.Input.ICommand AddTagCommand { get; }
    public System.Windows.Input.ICommand RemoveTagCommand { get; }

    public MSEntityMetadata(MSEntity msEntity)
    {
        _msEntity = msEntity;
        AddTagCommand = new RelayCommand(_ => AddTag(NewTag));
        RemoveTagCommand = new RelayCommand(p => RemoveTag(p as string ?? string.Empty));
        Refresh();
    }

    public void Refresh()
    {
        var entities = _msEntity.SelectedEntities;
        if (entities.Count == 0)
        {
            _isPlayer = null;
            _description = null;
            _sharedTags = null;
            _hasSharedTagsValue = false;
        }
        else
        {
            var metadataList = entities
                .Select(e => e.GetComponent<EntityMetadata>())
                .ToList();

            _isPlayer = AggregateBool(metadataList, m => m?.IsPlayer ?? false);
            _description = AggregateString(metadataList, m => m?.Description ?? string.Empty);
            _sharedTags = AggregateTags(metadataList, out _hasSharedTagsValue);
        }

        OnPropertyChanged(nameof(IsPlayer));
        OnPropertyChanged(nameof(Description));
        OnPropertyChanged(nameof(SharedTags));
        OnPropertyChanged(nameof(NewTag));
    }

    private static bool? AggregateBool(List<EntityMetadata?> list, Func<EntityMetadata, bool> sel)
    {
        bool firstValue = false;
        bool hasFirst = false;
        foreach (var m in list)
        {
            if (m == null) continue;
            if (!hasFirst) { firstValue = sel(m); hasFirst = true; continue; }
            if (sel(m) != firstValue) return null;
        }
        return hasFirst ? firstValue : (bool?)null;
    }

    private static string? AggregateString(List<EntityMetadata?> list, Func<EntityMetadata, string> sel)
    {
        string firstValue = string.Empty;
        bool hasFirst = false;
        foreach (var m in list)
        {
            if (m == null) continue;
            if (!hasFirst) { firstValue = sel(m); hasFirst = true; continue; }
            if (sel(m) != firstValue) return null;
        }
        return hasFirst ? firstValue : null;
    }

    private static System.Collections.Generic.List<string>? AggregateTags(List<EntityMetadata?> list, out bool hasShared)
    {
        System.Collections.Generic.List<string>? firstList = null;
        hasShared = false;
        foreach (var m in list)
        {
            if (m == null) continue;
            var tags = m.Tags.ToList();
            if (firstList == null) { firstList = tags; hasShared = true; continue; }
            if (tags.Count != firstList.Count || !tags.SequenceEqual(firstList))
            {
                hasShared = false;
                break;
            }
        }
        return firstList;
    }

    private void ApplyIsPlayer()
    {
        if (!_isPlayer.HasValue) return;
        foreach (var e in _msEntity.SelectedEntities)
        {
            var m = EnsureMetadata(e);
            m.IsPlayer = _isPlayer.Value;
        }
    }

    private void ApplyDescription()
    {
        if (_description == null) return;
        foreach (var e in _msEntity.SelectedEntities)
        {
            var m = EnsureMetadata(e);
            m.Description = _description;
        }
    }

    private void AddTag(string? tag)
    {
        if (string.IsNullOrWhiteSpace(tag)) return;
        var trimmed = tag.Trim();
        foreach (var e in _msEntity.SelectedEntities)
        {
            var m = EnsureMetadata(e);
            if (!m.Tags.Contains(trimmed)) m.Tags.Add(trimmed);
        }
        NewTag = string.Empty;
        OnPropertyChanged(nameof(NewTag));
        Refresh();
    }

    private void RemoveTag(string tag)
    {
        if (string.IsNullOrEmpty(tag)) return;
        foreach (var e in _msEntity.SelectedEntities)
        {
            var m = e.GetComponent<EntityMetadata>();
            if (m == null) continue;
            m.Tags.Remove(tag);
        }
        Refresh();
    }

    private static EntityMetadata EnsureMetadata(GameEntity entity)
    {
        var existing = entity.GetComponent<EntityMetadata>();
        if (existing != null) return existing;
        var fresh = new EntityMetadata();
        entity.Components.Add(fresh);
        return fresh;
    }
}
