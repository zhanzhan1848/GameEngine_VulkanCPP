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

    private readonly Stack<List<IUndoRedoAction>> _transactionStack = new();
    private int _isUndoingOrRedoing;

    public UndoRedo(int stackLimit = DefaultStackLimit)
    {
        _stackLimit = stackLimit;
        UndoStack = new ReadOnlyObservableCollection<IUndoRedoAction>(_undoStack);
        RedoStack = new ReadOnlyObservableCollection<IUndoRedoAction>(_redoStack);
    }

    public ReadOnlyObservableCollection<IUndoRedoAction> UndoStack { get; }
    public ReadOnlyObservableCollection<IUndoRedoAction> RedoStack { get; }

    public bool IsInTransaction => _transactionStack.Count > 0;

    public bool IsUndoingOrRedoing => _isUndoingOrRedoing > 0;

    public void Add(IUndoRedoAction action)
    {
        if (action == null) return;

        if (_transactionStack.Count > 0)
        {
            _transactionStack.Peek().Add(action);
            return;
        }

        _undoStack.Add(action);
        _redoStack.Clear();
        while (_undoStack.Count > _stackLimit)
        {
            _undoStack.RemoveAt(0);
        }
    }

    /// <summary>
    /// Begin a transactional scope. All <see cref="Add"/> calls inside the
    /// using-block are collected and committed as a single
    /// <see cref="TransactionAction"/> when the scope disposes.
    /// Nested calls are supported; only the outermost commit pushes to the
    /// undo stack.
    /// </summary>
    public IDisposable BeginTransaction(string name)
    {
        _transactionStack.Push(new List<IUndoRedoAction>());
        return new TransactionScope(this, name);
    }

    public void Undo()
    {
        if (_undoStack.Count == 0) return;
        if (_transactionStack.Count > 0) return; // mid-transaction: defer
        var action = _undoStack[^1];
        _undoStack.RemoveAt(_undoStack.Count - 1);
        _isUndoingOrRedoing++;
        try { action.Undo(); }
        finally { _isUndoingOrRedoing--; }
        _redoStack.Add(action);
    }

    public void Redo()
    {
        if (_redoStack.Count == 0) return;
        if (_transactionStack.Count > 0) return; // mid-transaction: defer
        var action = _redoStack[^1];
        _redoStack.RemoveAt(_redoStack.Count - 1);
        _isUndoingOrRedoing++;
        try { action.Redo(); }
        finally { _isUndoingOrRedoing--; }
        _undoStack.Add(action);
    }

    public void Reset()
    {
        _undoStack.Clear();
        _redoStack.Clear();
        _transactionStack.Clear();
    }

    private void CommitTransaction(string name)
    {
        if (_transactionStack.Count == 0) return;
        var actions = _transactionStack.Pop();
        if (_transactionStack.Count > 0)
        {
            // Nested: lift children into the outer scope unchanged.
            foreach (var a in actions) _transactionStack.Peek().Add(a);
            return;
        }
        if (actions.Count == 0) return;
        if (actions.Count == 1)
        {
            AddToUndoStack(actions[0]);
        }
        else
        {
            AddToUndoStack(new TransactionAction(name, actions));
        }
    }

    private void AbortTransaction()
    {
        if (_transactionStack.Count == 0) return;
        _transactionStack.Pop();
    }

    private void AddToUndoStack(IUndoRedoAction action)
    {
        _undoStack.Add(action);
        _redoStack.Clear();
        while (_undoStack.Count > _stackLimit)
        {
            _undoStack.RemoveAt(0);
        }
    }

    private sealed class TransactionScope : IDisposable
    {
        private readonly UndoRedo _owner;
        private readonly string _name;
        private bool _disposed;

        internal TransactionScope(UndoRedo owner, string name)
        {
            _owner = owner;
            _name = name;
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            _owner.CommitTransaction(_name);
        }

        internal void Abort()
        {
            if (_disposed) return;
            _disposed = true;
            _owner.AbortTransaction();
        }
    }

    /// <summary>
    /// Aggregate action that bundles several child actions into one
    /// undo/redo unit. Children are undone in reverse order and redone
    /// in original order.
    /// </summary>
    public sealed class TransactionAction : IUndoRedoAction
    {
        private readonly IReadOnlyList<IUndoRedoAction> _children;

        public TransactionAction(string name, IReadOnlyList<IUndoRedoAction> children)
        {
            Name = name;
            _children = children;
        }

        public string Name { get; }

        public void Undo()
        {
            for (int i = _children.Count - 1; i >= 0; --i) _children[i].Undo();
        }

        public void Redo()
        {
            for (int i = 0; i < _children.Count; ++i) _children[i].Redo();
        }
    }

    /// <summary>
    /// Helper that aborts a transaction (drops collected actions) on dispose
    /// instead of committing. Use via <c>using var scope = UndoRedo.BeginAbortableTransaction(...)</c>.
    /// </summary>
    public IDisposable BeginAbortableTransaction(string name)
    {
        _transactionStack.Push(new List<IUndoRedoAction>());
        return new AbortableTransactionScope(this, name);
    }

    private sealed class AbortableTransactionScope : IDisposable
    {
        private readonly UndoRedo _owner;
        private bool _disposed;

        internal AbortableTransactionScope(UndoRedo owner, string name)
        {
            _owner = owner;
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            _owner.AbortTransaction();
        }
    }
}

/// <summary>
/// Function-based undoable action. Use this when a single property
/// change (e.g. flipping IsSRGB on a Texture) can be expressed as a
/// pair of delegate invocations rather than a bespoke class.
/// </summary>
public sealed class DelegateAction : IUndoRedoAction
{
    public string Name { get; }
    private readonly System.Action _undo;
    private readonly System.Action _redo;
    public DelegateAction(string name, System.Action undo, System.Action redo)
    {
        if (string.IsNullOrEmpty(name)) name = "Change";
        Name = name;
        _undo = undo ?? throw new System.ArgumentNullException(nameof(undo));
        _redo = redo ?? throw new System.ArgumentNullException(nameof(redo));
    }
    public void Undo() => _undo();
    public void Redo() => _redo();
}