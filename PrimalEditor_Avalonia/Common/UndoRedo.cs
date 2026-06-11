using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;

namespace PrimalEditor_Avalonia.Common;

public interface IUndoRedoAction
{
    string Name { get; }
    void Undo();
    void Redo();
}

public class UndoRedo
{
    private const int DefaultStackLimit = 100;
    private readonly ObservableCollection<IUndoRedoAction> _undoStack = new();
    private readonly ObservableCollection<IUndoRedoAction> _redoStack = new();
    private readonly int _stackLimit;

    public UndoRedo(int stackLimit = DefaultStackLimit)
    {
        _stackLimit = stackLimit;
        UndoStack = new ReadOnlyObservableCollection<IUndoRedoAction>(_undoStack);
        RedoStack = new ReadOnlyObservableCollection<IUndoRedoAction>(_redoStack);
    }

    public ReadOnlyObservableCollection<IUndoRedoAction> UndoStack { get; }
    public ReadOnlyObservableCollection<IUndoRedoAction> RedoStack { get; }

    public void Add(IUndoRedoAction action)
    {
        if (action == null) return;
        _undoStack.Add(action);
        _redoStack.Clear();
        while (_undoStack.Count > _stackLimit)
        {
            _undoStack.RemoveAt(0);
        }
    }

    public void Undo()
    {
        if (_undoStack.Count == 0) return;
        var action = _undoStack[^1];
        _undoStack.RemoveAt(_undoStack.Count - 1);
        action.Undo();
        _redoStack.Add(action);
    }

    public void Redo()
    {
        if (_redoStack.Count == 0) return;
        var action = _redoStack[^1];
        _redoStack.RemoveAt(_redoStack.Count - 1);
        action.Redo();
        _undoStack.Add(action);
    }

    public void Reset()
    {
        _undoStack.Clear();
        _redoStack.Clear();
    }
}