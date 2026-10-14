using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Threading;
using Primal.CliFramework;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Sequence;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Cli;

/// <summary>
/// Sequence 拍片命令面（Docs/2026-09-05 Part A，对标 Sequencer/Movie Render
/// Queue 的一期）：sequence.new/open/save/info 管生命周期；add-track/add-key/
/// add-camera-cut 等编辑命令与时间轴面板走同一 UndoRedo 路径（可 Ctrl+Z）；
/// play/pause/stop/set-frame 是播放态（不入 undo）；sequence.render 是
/// Movie Render Queue 一期——编辑器进程内逐帧确定性离屏渲染。
/// 时间为帧号轴，单位米/度（与 Transform 组件同契约）。
/// </summary>
public sealed class SequenceCommands
{
    private const int MaxRenderFrames = 3600;

    private static WorldEditorViewModel RequireWorldEditor() =>
        EditorServices.WorldEditor
        ?? throw CliError.NotReady("编辑器视图中没有世界编辑器实例", "先在编辑器中打开项目");

    private static Project RequireProject() =>
        RequireWorldEditor().Project
        ?? throw CliError.NotReady("当前没有打开的项目", "先在编辑器中打开项目（editor.ping 查看 project 字段）");

    private static SequenceDocument RequireOpenSequence()
    {
        var doc = SequenceEditorState.Current;
        if (doc == null)
            throw CliError.NotReady("当前没有打开的序列", "先 sequence.new 或 sequence.open");
        return doc;
    }

    // ===================================================================
    // 生命周期
    // ===================================================================

    [CliCommand("sequence.new",
        Description = "新建拍片序列并打开（对标 Level Sequence）。fps 为帧率 1..120（帧↔秒换算与输出速率），" +
                      "durationFrames 为序列长度 ≥1（有效帧区间 [0, durationFrames)）。" +
                      "时间轴是帧号轴，单位米/度。新建仅入内存不落盘——用 sequence.save 持久化到 " +
                      "<project>/Sequences/<name>.pseq。新建即替换当前打开的序列（如有）。",
        Example = """{"name":"开场运镜","fps":30,"durationFrames":120}""")]
    public object New(string? name = null, int fps = 30, int durationFrames = 120)
    {
        RequireProject();
        if (fps is < 1 or > 120)
            throw CliError.InvalidParams("fps 取值 1..120", "24/30/60 是常用档位");
        if (durationFrames < 1)
            throw CliError.InvalidParams("durationFrames 必须 ≥ 1");
        var doc = new SequenceDocument
        {
            Name = string.IsNullOrWhiteSpace(name) ? "Sequence" : name!.Trim(),
            Fps = fps,
            DurationFrames = durationFrames,
        };
        SequenceEditorState.Open(doc, path: null);
        return Describe(doc, created: true);
    }

    [CliCommand("sequence.open",
        Description = "打开 .pseq 序列文件（相对路径按项目根解析，也可绝对路径）。" +
                      "文件不存在/JSON 非法返回相应错误。打开即替换当前打开的序列（如有）。",
        Example = """{"path":"Sequences/开场运镜.pseq"}""")]
    public object Open(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw CliError.InvalidParams("path 不能为空", @"示例: {""path"":""Sequences/intro.pseq""}");
        var project = RequireProject();
        var resolved = ResolveSequencePath(project, path!);
        if (!File.Exists(resolved))
            throw CliError.NotFound($"序列文件 '{path}'",
                "项目内可用序列见 " + SequenceDocument.GetDirectory(project.Path));
        SequenceDocument doc;
        try
        {
            doc = SequenceDocument.Load(resolved);
        }
        catch (InvalidOperationException ex)
        {
            throw new CliError(CliErrorCodes.EngineError, ex.Message, "确认文件由 sequence.save 生成");
        }
        SequenceEditorState.Open(doc, resolved);
        return Describe(doc, created: false);
    }

    [CliCommand("sequence.save",
        Description = "保存当前打开的序列。path 可选（相对项目根或绝对路径）；省略时用当前路径，" +
                      "首次保存缺省 <project>/Sequences/<name>.pseq。返回落盘路径。",
        Example = """{}""")]
    public object Save(string? path = null)
    {
        var doc = RequireOpenSequence();
        var project = RequireProject();
        var resolved = string.IsNullOrWhiteSpace(path)
            ? SequenceEditorState.CurrentPath ?? DefaultPath(project, doc)
            : ResolveSequencePath(project, path!);
        doc.Save(resolved);
        SequenceEditorState.Open(doc, resolved); // 记住落盘路径
        return new { saved = true, path = resolved };
    }

    [CliCommand("sequence.info",
        Description = "查看当前打开序列的摘要：fps/时长/播放头/轨道列表（变换轨的实体 guid 与关键帧数、" +
                      "相机 cut 轨的 shot 列表）。没有打开序列返回 NOT_READY。",
        Example = "{}")]
    public object Info() => Describe(RequireOpenSequence(), created: false);

    // ===================================================================
    // 轨道编辑（记入 undo）
    // ===================================================================

    [CliCommand("sequence.add-transform-track",
        Description = "给实体加变换关键帧轨（位置/旋转/缩放，单位米/度，右手系 +Y 向上）。" +
                      "实体必须存在于当前场景（guid 用 scene.tree 查）；已有同实体轨返回 NOT_FOUND 语义错误。" +
                      "记入 undo。相机运镜 = 给相机实体加本轨。",
        Example = """{"entityGuid":"…"}""")]
    public object AddTransformTrack(Guid entityGuid)
    {
        var doc = RequireOpenSequence();
        var worldEditor = RequireWorldEditor();
        RequireEntity(worldEditor, entityGuid);
        if (doc.FindTransformTrack(entityGuid) != null)
            throw new CliError(CliErrorCodes.InvalidParams,
                "该实体已有变换轨", "sequence.info 查看现有轨道；一个实体一条变换轨");

        var track = new TransformTrack { EntityGuid = entityGuid, Name = TrackName(worldEditor, entityGuid) };
        PushUndo(worldEditor, "Sequence Add Track",
            new SequenceUndoActions.AddTrackAction(doc, track));
        return Describe(doc, created: false);
    }

    [CliCommand("sequence.add-camera-cut",
        Description = "加一个分镜头（camera cut）：[startFrame, endFrame) 半开区间由 cameraEntityGuid " +
                      "取景（endFrame 不含，相邻 shot 不重叠）。相机实体须有 Camera 组件。记入 undo。 " +
                      "无 cut 覆盖的帧回落编辑器相机。",
        Example = """{"startFrame":0,"endFrame":48,"cameraEntityGuid":"…"}""")]
    public object AddCameraCut(int startFrame, int endFrame, Guid cameraEntityGuid)
    {
        var doc = RequireOpenSequence();
        var worldEditor = RequireWorldEditor();
        var entity = RequireEntity(worldEditor, cameraEntityGuid);
        if (entity.Components.OfType<CameraComponent>().FirstOrDefault() == null)
            throw CliError.InvalidParams("cameraEntityGuid 指向的实体没有 Camera 组件",
                "在编辑器中给实体添加 Camera 组件后再试");
        if (startFrame < 0 || endFrame <= startFrame)
            throw CliError.InvalidParams("要求 0 ≤ startFrame < endFrame",
                "半开区间示例: {\"startFrame\":0,\"endFrame\":48}");
        var clampedEnd = Math.Min(endFrame, doc.DurationFrames);
        if (clampedEnd <= startFrame)
            throw CliError.InvalidParams($"endFrame 超出序列长度（durationFrames={doc.DurationFrames}）",
                "先调大 sequence.new 的 durationFrames");

        var cut = doc.CameraCut;
        var actions = new List<IUndoRedoAction>();
        if (cut == null)
        {
            cut = new CameraCutTrack { Name = "Camera Cuts" };
            actions.Add(new SequenceUndoActions.AddTrackAction(doc, cut));
        }
        actions.Add(new SequenceUndoActions.AddShotAction(cut,
            new CameraShot { StartFrame = startFrame, EndFrame = clampedEnd, CameraEntityGuid = cameraEntityGuid }));
        PushUndo(worldEditor, "Sequence Add Camera Cut", actions.ToArray());
        return Describe(doc, created: false);
    }

    [CliCommand("sequence.add-key",
        Description = "给实体变换轨加关键帧（无轨时自动建轨）。frame 缺省 = 当前播放头；" +
                      "position/rotationEuler/scale（Vec3，米/度）可省——省略的字段取实体当前值" +
                      "（即对齐当前视角的 K 帧）。同帧已有 key 则替换。interp: Linear|Constant（缺省 Linear）。记入 undo。",
        Example = """{"entityGuid":"…","frame":12,"position":[1,0,3]}""")]
    public object AddKey(Guid entityGuid, int? frame = null, Vec3? position = null,
        Vec3? rotationEuler = null, Vec3? scale = null, string? interp = null)
    {
        var doc = RequireOpenSequence();
        var worldEditor = RequireWorldEditor();
        var entity = RequireEntity(worldEditor, entityGuid);
        var transform = entity.GetTransform()
            ?? throw CliError.NotFound($"实体 '{entity.Name}' 没有 Transform 组件", "scene.tree 查看组件列表");
        var at = frame.HasValue
            ? Math.Clamp(frame.Value, 0, doc.DurationFrames - 1)
            : SequenceEditorState.CurrentFrame;

        if (scale is { } s && (s.X <= 0 || s.Y <= 0 || s.Z <= 0))
            throw CliError.InvalidParams("scale 分量必须 > 0");
        var interpolation = ParseInterp(interp);
        var cur = TransformData.FromTransform(transform);

        var track = doc.FindTransformTrack(entityGuid);
        var actions = new List<IUndoRedoAction>();
        if (track == null)
        {
            track = new TransformTrack { EntityGuid = entityGuid, Name = TrackName(worldEditor, entityGuid) };
            actions.Add(new SequenceUndoActions.AddTrackAction(doc, track));
        }
        var key = new TransformKey
        {
            Frame = at,
            Position = position is { } p ? new[] { p.X, p.Y, p.Z } : new[] { cur.PositionX, cur.PositionY, cur.PositionZ },
            RotationEuler = rotationEuler is { } r ? new[] { r.X, r.Y, r.Z } : new[] { cur.RotationX, cur.RotationY, cur.RotationZ },
            Scale = scale is { } sc ? new[] { sc.X, sc.Y, sc.Z } : new[] { cur.ScaleX, cur.ScaleY, cur.ScaleZ },
            Interpolation = interpolation,
        };
        actions.Add(new SequenceUndoActions.AddKeyAction(track, key));
        PushUndo(worldEditor, "Sequence Add Key", actions.ToArray());
        return new { trackEntityGuid = entityGuid.ToString("D").ToLowerInvariant(), frame = at, keys = track.Keys.Count };
    }

    [CliCommand("sequence.remove-key",
        Description = "删除实体变换轨上指定帧的关键帧。轨不存在或该帧无 key 返回 NOT_FOUND。记入 undo。",
        Example = """{"entityGuid":"…","frame":12}""")]
    public object RemoveKey(Guid entityGuid, int frame)
    {
        var doc = RequireOpenSequence();
        var worldEditor = RequireWorldEditor();
        var track = doc.FindTransformTrack(entityGuid)
            ?? throw CliError.NotFound($"实体 {entityGuid} 的变换轨", "sequence.info 查看现有轨道");
        var key = track.Keys.FirstOrDefault(k => k.Frame == frame)
            ?? throw CliError.NotFound($"帧 {frame} 上的关键帧", "sequence.info 查看关键帧分布");
        PushUndo(worldEditor, "Sequence Remove Key",
            new SequenceUndoActions.RemoveKeyAction(track, key));
        return new { removed = true, frame, keys = track.Keys.Count };
    }

    [CliCommand("sequence.set-property-key",
        Description = "给实体属性轨设关键帧（sequence S1；无轨时自动建轨）。component.property 白名单：" +
                      "light.intensity（标量，量纲同 Light 组件）。同帧已有 key 替换；frame 缺省 = 当前播放头。" +
                      "interp: Linear|Constant（缺省 Linear）。目标实体缺对应组件返回 INVALID_PARAMS。记入 undo。",
        Example = """{"entityGuid":"…","frame":0,"value":0}""")]
    public object SetPropertyKey(Guid entityGuid, int? frame, float value,
        string component = "light", string property = "intensity", string? interp = null)
    {
        var doc = RequireOpenSequence();
        var worldEditor = RequireWorldEditor();
        if (!PropertyTrack.IsSupported(component, property))
            throw CliError.InvalidParams($"不支持的属性 {component}.{property}",
                "支持面: " + string.Join(", ", PropertyTrack.SupportedProperties.Select(p => $"{p.Component}.{p.Property}")));
        var entity = RequireEntity(worldEditor, entityGuid);
        if (component == "light" && entity.GetComponent<LightComponent>() == null)
            throw CliError.InvalidParams($"实体 '{entity.Name}' 没有 Light 组件", "component.add-light 先加组件");
        var at = frame.HasValue
            ? Math.Clamp(frame.Value, 0, doc.DurationFrames - 1)
            : SequenceEditorState.CurrentFrame;

        var track = doc.FindPropertyTrack(entityGuid, component, property);
        var actions = new List<IUndoRedoAction>();
        if (track == null)
        {
            track = new PropertyTrack
            {
                EntityGuid = entityGuid,
                Component = component,
                Property = property,
                Name = $"{TrackName(worldEditor, entityGuid)} · {component}.{property}",
            };
            actions.Add(new SequenceUndoActions.AddTrackAction(doc, track));
        }
        actions.Add(new SequenceUndoActions.AddPropertyKeyAction(
            track, new PropertyKey { Frame = at, Value = value, Interpolation = ParseInterp(interp) }));
        PushUndo(worldEditor, "Sequence Set Property Key", actions.ToArray());
        return new
        {
            entityGuid = entityGuid.ToString("D").ToLowerInvariant(),
            component,
            property,
            frame = at,
            value,
            keys = track.Keys.Count,
        };
    }

    [CliCommand("sequence.remove-property-key",
        Description = "删除属性轨上指定帧的关键帧（sequence S1）。轨不存在或该帧无 key 返回 NOT_FOUND。记入 undo。",
        Example = """{"entityGuid":"…","frame":12}""")]
    public object RemovePropertyKey(Guid entityGuid, int frame,
        string component = "light", string property = "intensity")
    {
        var doc = RequireOpenSequence();
        var worldEditor = RequireWorldEditor();
        var track = doc.FindPropertyTrack(entityGuid, component, property)
            ?? throw CliError.NotFound($"实体 {entityGuid} 的 {component}.{property} 轨",
                "sequence.info 查看现有轨道");
        var key = track.Keys.FirstOrDefault(k => k.Frame == frame)
            ?? throw CliError.NotFound($"帧 {frame} 上的属性关键帧", "sequence.info 查看关键帧分布");
        PushUndo(worldEditor, "Sequence Remove Property Key",
            new SequenceUndoActions.RemovePropertyKeyAction(track, key));
        return new { removed = true, frame, keys = track.Keys.Count };
    }

    [CliCommand("sequence.remove-track",
        Description = "删除实体变换轨（含全部关键帧）。轨不存在返回 NOT_FOUND。记入 undo。",
        Example = """{"entityGuid":"…"}""")]
    public object RemoveTrack(Guid entityGuid)
    {
        var doc = RequireOpenSequence();
        var worldEditor = RequireWorldEditor();
        var track = doc.FindTransformTrack(entityGuid)
            ?? throw CliError.NotFound($"实体 {entityGuid} 的变换轨", "sequence.info 查看现有轨道");
        PushUndo(worldEditor, "Sequence Remove Track",
            new SequenceUndoActions.RemoveTrackAction(doc, track));
        return new { removed = true };
    }

    // ===================================================================
    // 播放态（不入 undo）
    // ===================================================================

    [CliCommand("sequence.set-frame",
        Description = "把播放头移到指定帧并把求值结果应用到场景（预览 scrub）。" +
                      "帧号自动 clamp 到 [0, durationFrames)——越界不报错，返回实际生效帧。" +
                      "播放态修改不入 undo；实体原 pose 在下一次编辑前不会自动恢复" +
                      "（undo 只针对关键帧编辑，不针对 seek）。",
        Example = """{"frame":30}""")]
    public object SetFrame(int frame)
    {
        var doc = RequireOpenSequence();
        SequenceEditorState.SetFrame(frame); // clamp 到 [0, durationFrames)
        var worldEditor = RequireWorldEditor();
        ApplyCurrent(worldEditor: worldEditor, doc);
        var snapshot = SequenceEvaluator.Evaluate(doc, SequenceEditorState.CurrentFrame);
        return new
        {
            frame = SequenceEditorState.CurrentFrame,
            properties = snapshot.Properties.Select(p => new
            {
                entityGuid = p.EntityGuid.ToString("D").ToLowerInvariant(),
                component = p.Component,
                property = p.Property,
                value = p.Value,
            }).ToArray(),
        };
    }

    [CliCommand("sequence.play",
        Description = "开始视口预览播放（DispatcherTimer @ 序列 fps 逐帧求值应用，相机 cut 自动切机位）。" +
                      "已在播放时幂等返回。播到末帧自动停。",
        Example = "{}")]
    public object Play()
    {
        var doc = RequireOpenSequence();
        Playback.Start();
        return new { playing = true, fps = doc.Fps };
    }

    [CliCommand("sequence.pause",
        Description = "暂停预览播放（播放头停在当前帧）。未在播放时幂等返回。",
        Example = "{}")]
    public object Pause()
    {
        RequireOpenSequence();
        Playback.Stop(rewind: false);
        return new { playing = false, frame = SequenceEditorState.CurrentFrame };
    }

    [CliCommand("sequence.stop",
        Description = "停止预览播放并回到第 0 帧（应用第 0 帧求值结果）。",
        Example = "{}")]
    public object Stop()
    {
        var doc = RequireOpenSequence();
        Playback.Stop(rewind: true);
        ApplyCurrent(RequireWorldEditor(), doc);
        return new { playing = false, frame = 0 };
    }

    // ===================================================================
    // 离线渲染（Movie Render Queue 一期）
    // ===================================================================

    [CliCommand("sequence.render",
        Description = "把当前序列离线渲染为 PNG 帧序列（frame_%04d.png，0 起）到 out 目录" +
                      "（绝对路径，自动创建）。逐帧：seek 求值应用 → 按相机 cut 取景（无 cut 帧用编辑器相机）" +
                      "→ 离屏渲染落盘；分辨率 = 3D 视口当前分辨率。确定性：时间只由帧号驱动，" +
                      "同序列两次渲染逐帧一致（可用 editor.diff 验证）。渲染前快照受影响实体 pose，" +
                      "结束后恢复（场景不留录制残留），播放头回到起始帧。可选 video=x.mp4 用外部 ffmpeg " +
                      "按序列 fps 编码（缺失时 videoEncoded=false，不影响 PNG）。帧数上限 3600。",
        Example = """{"out":"/tmp/render","video":"/tmp/render/intro.mp4"}""")]
    public async Task<object> Render(string @out, string? video = null)
    {
        var doc = RequireOpenSequence();
        var worldEditor = RequireWorldEditor();
        var viewport = EditorServices.FindViewport()
            ?? throw CliError.NotReady("3D 视口未就绪", "打开项目并切换到世界编辑器标签后再渲染");
        if (string.IsNullOrWhiteSpace(@out))
            throw CliError.InvalidParams("out 不能为空", @"示例: {""out"":""/tmp/render""}");
        if (doc.DurationFrames > MaxRenderFrames)
            throw CliError.InvalidParams($"durationFrames={doc.DurationFrames} 超过上限 {MaxRenderFrames}",
                "长序列分段渲染或拆分镜头");

        var dir = Path.GetFullPath(@out!);
        Directory.CreateDirectory(dir);
        RequireProject();

        // 播放态互斥 + 快照：渲染结束后场景与播放头都回到渲染前状态。
        Playback.Stop(rewind: false);
        var poses = SequenceDriver.CapturePoses(doc, worldEditor);
        var restoreFrame = SequenceEditorState.CurrentFrame;
        var perFrameMs = new List<double>(doc.DurationFrames);
        var files = new List<string>(doc.DurationFrames);
        int width = 0, height = 0;

        try
        {
            for (int frame = 0; frame < doc.DurationFrames; frame++)
            {
                var sw = System.Diagnostics.Stopwatch.StartNew();
                SequenceEditorState.SetFrame(frame);
                var (_, cameraNativeId) = SequenceDriver.ApplyFrame(doc, frame, worldEditor);
                var path = Path.Combine(dir, $"frame_{frame:D4}.png");
                if (!viewport.TryRenderSequencePng(path, cameraNativeId, out width, out height))
                    throw new CliError(CliErrorCodes.EngineError,
                        $"第 {frame} 帧渲染失败（视口未就绪或引擎忙）",
                        "先 editor.ping 确认 engineInitialized，稍候重试；已写出的帧保留在输出目录");
                files.Add(path);
                perFrameMs.Add(sw.Elapsed.TotalMilliseconds);

                // 让出 UI 线程一拍排空 socket 队列（editor.metrics 同款——否则
                // 整个渲染循环独占主线程）。
                await Dispatcher.UIThread.InvokeAsync(() => { });
            }
        }
        finally
        {
            SequenceDriver.RestorePoses(poses, worldEditor);
            SequenceEditorState.SetFrame(restoreFrame);
        }

        string? videoPath = null;
        bool videoEncoded = false;
        string? videoHint = null;
        if (!string.IsNullOrWhiteSpace(video))
        {
            videoPath = Path.IsPathRooted(video!) ? video : Path.GetFullPath(video!);
            (videoEncoded, videoHint) = VideoEncoder.EncodePngSequence(dir, videoPath, width, height, doc.Fps);
        }

        return new
        {
            dir,
            frames = files.Count,
            width,
            height,
            fps = doc.Fps,
            avgFrameMs = Math.Round(perFrameMs.Count == 0 ? 0 : perFrameMs.Average(), 2),
            totalMs = Math.Round(perFrameMs.Sum(), 1),
            video = videoPath,
            videoEncoded,
            videoHint,
        };
    }

    // ===================================================================
    // 内部
    // ===================================================================

    /// <summary>
    /// 预览播放器（面板与 CLI 共用 <see cref="Playback"/> 单例）：DispatcherTimer
    /// @ 序列 fps，每 tick 前进一帧并求值应用；播到末帧自动停。timer 唯一——
    /// 不存在两个播放驱动互相打架。
    /// </summary>
    public sealed class SequencerController
    {
        private DispatcherTimer? _timer;

        public bool IsRunning => _timer != null;

        /// <summary>播放停止（含播到末帧自动停）时通知，面板据此刷新传输条。</summary>
        public event Action? PlaybackStopped;

        public void Start()
        {
            var doc = SequenceEditorState.Current;
            if (doc == null || _timer != null) return; // 幂等
            if (SequenceEditorState.CurrentFrame >= doc.DurationFrames - 1)
                SequenceEditorState.SetFrame(0);
            if (EditorServices.WorldEditor is { } world) ApplyCurrent(world, doc);
            SequenceEditorState.SetPlaying(true);
            _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1000.0 / Math.Max(1, doc.Fps)) };
            _timer.Tick += (_, _) =>
            {
                var d = SequenceEditorState.Current;
                if (d == null || SequenceEditorState.CurrentFrame + 1 >= d.DurationFrames)
                {
                    Stop(rewind: false);
                    return;
                }
                SequenceEditorState.SetFrame(SequenceEditorState.CurrentFrame + 1);
                if (EditorServices.WorldEditor is { } we) ApplyCurrent(we, d);
            };
            _timer.Start();
        }

        /// <summary>停止播放；rewind=true 时回到第 0 帧。</summary>
        public void Stop(bool rewind)
        {
            var wasRunning = _timer != null;
            _timer?.Stop();
            _timer = null;
            SequenceEditorState.SetPlaying(false);
            if (rewind) SequenceEditorState.SetFrame(0);
            if (wasRunning) PlaybackStopped?.Invoke();
        }
    }

    /// <summary>预览播放器单例（面板与 CLI 共用）。</summary>
    public static SequencerController Playback { get; } = new();

    private static void ApplyCurrent(WorldEditorViewModel worldEditor, SequenceDocument doc)
    {
        var (_, camera) = SequenceDriver.ApplyFrame(doc, SequenceEditorState.CurrentFrame, worldEditor);
        var viewport = EditorServices.FindViewport();
        if (viewport != null && viewport.LookThroughSceneEntityId != camera)
            viewport.LookThroughCamera(camera);
    }

    private static void PushUndo(WorldEditorViewModel worldEditor, string name, params IUndoRedoAction[] actions)
    {
        // 事务合成一步：add-key 自动建轨这类复合操作 = 一个 undo 步
        //（与 UI 粘贴/复制实体的 BeginTransaction 惯例一致）。
        using (worldEditor.UndoRedo.BeginTransaction(name))
        {
            foreach (var action in actions)
            {
                worldEditor.UndoRedo.Add(action);
                action.Redo();
            }
        }
        SequenceEditorState.FireChanged();
    }

    private static GameEntity RequireEntity(WorldEditorViewModel worldEditor, Guid guid)
    {
        var scene = EditorServices.ActiveScene
            ?? throw CliError.NotReady("当前没有激活场景", "先 scene.load 或 scene.new");
        return SequenceDriver.FindEntity(scene.Entities, guid)
            ?? throw CliError.NotFound($"实体 {guid}", "scene.tree 查看实体 guid");
    }

    private static string TrackName(WorldEditorViewModel worldEditor, Guid guid) =>
        SequenceDriver.FindEntity(EditorServices.ActiveScene?.Entities, guid)?.Name ?? "Entity";

    private static KeyInterpolation ParseInterp(string? interp)
    {
        if (string.IsNullOrWhiteSpace(interp)) return KeyInterpolation.Linear;
        return interp!.Trim().ToLowerInvariant() switch
        {
            "linear" => KeyInterpolation.Linear,
            "constant" => KeyInterpolation.Constant,
            _ => throw CliError.InvalidParams($"未知插值 '{interp}'", "有效取值: Linear | Constant"),
        };
    }

    private static string ResolveSequencePath(Project project, string path)
    {
        if (Path.IsPathRooted(path)) return Path.GetFullPath(path);
        return Path.GetFullPath(Path.Combine(project.Path, path));
    }

    private static string DefaultPath(Project project, SequenceDocument doc) =>
        Path.Combine(SequenceDocument.GetDirectory(project.Path),
            string.Concat(doc.Name.Select(c => Path.GetInvalidFileNameChars().Contains(c) ? '_' : c)) + ".pseq");

    private static object Describe(SequenceDocument doc, bool created) => new
    {
        name = doc.Name,
        fps = doc.Fps,
        durationFrames = doc.DurationFrames,
        path = SequenceEditorState.CurrentPath,
        frame = SequenceEditorState.CurrentFrame,
        playing = SequenceEditorState.IsPlaying,
        created,
        tracks = doc.Tracks.Select(t => t switch
        {
            TransformTrack tt => (object)new
            {
                type = "transform",
                name = tt.Name,
                entityGuid = tt.EntityGuid.ToString("D").ToLowerInvariant(),
                keyCount = tt.Keys.Count,
                firstFrame = tt.Keys.Count > 0 ? tt.Keys[0].Frame : -1,
                lastFrame = tt.Keys.Count > 0 ? tt.Keys[^1].Frame : -1,
            },
            CameraCutTrack ct => new
            {
                type = "cameraCut",
                name = ct.Name,
                shots = ct.Shots.Select(s => new
                {
                    startFrame = s.StartFrame,
                    endFrame = s.EndFrame,
                    cameraEntityGuid = s.CameraEntityGuid.ToString("D").ToLowerInvariant(),
                }).ToArray(),
            },
            PropertyTrack pt => new
            {
                type = "property",
                name = pt.Name,
                entityGuid = pt.EntityGuid.ToString("D").ToLowerInvariant(),
                component = pt.Component,
                property = pt.Property,
                keyCount = pt.Keys.Count,
                firstFrame = pt.Keys.Count > 0 ? pt.Keys[0].Frame : -1,
                lastFrame = pt.Keys.Count > 0 ? pt.Keys[^1].Frame : -1,
            },
            _ => new { type = t.GetType().Name },
        }).ToArray(),
    };
}
