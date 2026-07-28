using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

public interface IMSComponent
{
    void Refresh();
}

public abstract class MSComponent<T> : ViewModelBase, IMSComponent where T : Component
{
    private bool _enableUpdates = true;

    protected MSEntity MSEntity { get; }
    public List<T> SelectedComponents { get; }

    protected abstract bool UpdateComponents(string propertyName);
    protected abstract bool UpdateMSComponent();

    protected void EnableUpdates(bool on) => _enableUpdates = on;

    protected MSComponent(MSEntity msEntity)
    {
        MSEntity = msEntity;
        SelectedComponents = msEntity.SelectedEntities
            .Select(e => e.GetComponent<T>())
            .Where(c => c != null)
            .Cast<T>()
            .ToList();
        PropertyChanged += (_, e) =>
        {
            if (_enableUpdates) UpdateComponents(e.PropertyName ?? string.Empty);
        };
    }

    public void Refresh()
    {
        EnableUpdates(false);
        UpdateMSComponent();
        EnableUpdates(true);
    }
}