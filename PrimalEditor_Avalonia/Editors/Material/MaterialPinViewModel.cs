using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using NodifyM.Avalonia.ViewModelBase;

namespace PrimalEditor_Avalonia.Editors.Material;

/// <summary>
/// One input/output pin on a <see cref="MaterialNodeViewModel"/>. Maps
/// 1-based pin index to a typed connector the graph editor can draw and
/// the user can drag to form connections.
/// </summary>
public sealed class MaterialPinViewModel : ConnectorViewModelBase
{
    public uint NodeId { get; init; }
    public uint PinIndex { get; init; }
    public uint DataType { get; init; }
    public bool IsInput { get; init; }
}