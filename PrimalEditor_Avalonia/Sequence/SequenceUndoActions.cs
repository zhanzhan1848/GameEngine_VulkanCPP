using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Sequence;

/// <summary>
/// Sequence 轨道/关键帧编辑的 undo 动作（文档模型优先纪律：编辑器面板与
/// CLI 共用同一动作、同一 UndoRedo 栈，Ctrl+Z 全量可撤销）。动作只改
/// SequenceDocument；求值应用（ApplyTo+PushTransformsToNative）由播放/
/// 渲染驱动器负责，不在这里——seek/播放态不入 undo。
/// </summary>
public static class SequenceUndoActions
{
    public sealed class AddTrackAction : IUndoRedoAction
    {
        private readonly SequenceDocument _doc;
        private readonly SequenceTrack _track;

        public string Name => $"Sequence Add Track '{_track.Name}'";
        public AddTrackAction(SequenceDocument doc, SequenceTrack track) { _doc = doc; _track = track; }

        public void Undo() => _doc.Tracks.Remove(_track);
        public void Redo()
        {
            if (!_doc.Tracks.Contains(_track)) _doc.Tracks.Add(_track);
        }
    }

    public sealed class RemoveTrackAction : IUndoRedoAction
    {
        private readonly SequenceDocument _doc;
        private readonly SequenceTrack _track;
        private readonly int _index;

        public string Name => $"Sequence Remove Track '{_track.Name}'";
        public RemoveTrackAction(SequenceDocument doc, SequenceTrack track)
        {
            _doc = doc; _track = track; _index = doc.Tracks.IndexOf(track);
        }

        public void Undo()
        {
            if (_index >= 0 && !_doc.Tracks.Contains(_track))
                _doc.Tracks.Insert(Math.Min(_index, _doc.Tracks.Count), _track);
        }
        public void Redo() => _doc.Tracks.Remove(_track);
    }

    /// <summary>同帧替换型：Undo 恢复原 key，Redo 重新插入新 key。</summary>
    public sealed class AddKeyAction : IUndoRedoAction
    {
        private readonly TransformTrack _track;
        private readonly TransformKey _key;
        private TransformKey? _replaced;

        public string Name => "Sequence Add Key";
        public AddKeyAction(TransformTrack track, TransformKey key) { _track = track; _key = key; }

        public void Undo()
        {
            _track.Keys.Remove(_key);
            if (_replaced != null) InsertSorted(_replaced);
        }
        public void Redo()
        {
            _replaced = null;
            for (int i = 0; i < _track.Keys.Count; i++)
                if (_track.Keys[i].Frame == _key.Frame) { _replaced = _track.Keys[i]; _track.Keys[i] = _key; return; }
            InsertSorted(_key);
        }
        private void InsertSorted(TransformKey key)
        {
            for (int i = 0; i < _track.Keys.Count; i++)
                if (_track.Keys[i].Frame > key.Frame) { _track.Keys.Insert(i, key); return; }
            _track.Keys.Add(key);
        }
    }

    public sealed class RemoveKeyAction : IUndoRedoAction
    {
        private readonly TransformTrack _track;
        private readonly TransformKey _key;
        private readonly int _index;

        public string Name => "Sequence Remove Key";
        public RemoveKeyAction(TransformTrack track, TransformKey key)
        {
            _track = track; _key = key; _index = track.Keys.IndexOf(key);
        }

        public void Undo()
        {
            if (_index >= 0 && !_track.Keys.Contains(_key))
                _track.Keys.Insert(Math.Min(_index, _track.Keys.Count), _key);
        }
        public void Redo() => _track.Keys.Remove(_key);
    }

    public sealed class AddShotAction : IUndoRedoAction
    {
        private readonly CameraCutTrack _track;
        private readonly CameraShot _shot;

        public string Name => "Sequence Add Camera Shot";
        public AddShotAction(CameraCutTrack track, CameraShot shot) { _track = track; _shot = shot; }

        public void Undo() => _track.Shots.Remove(_shot);
        public void Redo()
        {
            if (!_track.Shots.Contains(_shot)) _track.Shots.Add(_shot);
        }
    }

    public sealed class RemoveShotAction : IUndoRedoAction
    {
        private readonly CameraCutTrack _track;
        private readonly CameraShot _shot;

        public string Name => "Sequence Remove Camera Shot";
        public RemoveShotAction(CameraCutTrack track, CameraShot shot) { _track = track; _shot = shot; }

        public void Undo()
        {
            if (!_track.Shots.Contains(_shot)) _track.Shots.Add(_shot);
        }
        public void Redo() => _track.Shots.Remove(_shot);
    }
}
