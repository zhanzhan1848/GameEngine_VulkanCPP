using System;
using System.Collections.Generic;
using System.ComponentModel;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>
/// Inspector-side view model for <see cref="SceneEnvironment"/>. Forwards
/// every property change to (a) the underlying <see cref="SceneEnvironment"/>
/// on the active scene, (b) <see cref="NativeEngineBridge.UpdateSceneEnvironment"/>,
/// and (c) marks the project dirty so Save catches it. The native push
/// is best-effort: if the engine isn't loaded it silently no-ops.
/// </summary>
public sealed class SceneEnvironmentViewModel : ViewModelBase
{
    private SceneEnvironment? _env;

    public bool HasScene => _env != null;

    public IReadOnlyList<ToneMappingMode> ToneMappingOptions { get; } =
        new[] { ToneMappingMode.None, ToneMappingMode.Reinhard, ToneMappingMode.ACES, ToneMappingMode.ExposureOnly };

    public string StatusMessage =>
        _env == null
            ? "No active scene — open or create a scene to edit its environment."
            : $"Scene: {_env.GetHashCode()} — last push: {(_lastPushOk ? "OK" : "no engine")}";

    private bool _lastPushOk;

    public void Attach(SceneEnvironment? env)
    {
        if (_env != null) _env.PropertyChanged -= OnEnvChanged;
        _env = env;
        if (_env != null)
        {
            _env.PropertyChanged += OnEnvChanged;
            ApplyToNative();
        }
        OnPropertyChanged(nameof(HasScene));
        OnPropertyChanged(nameof(StatusMessage));
        // Re-fire every individual property so the view re-binds.
        foreach (var p in new[]
            {
                nameof(AmbientR), nameof(AmbientG), nameof(AmbientB), nameof(AmbientIntensity),
                nameof(FogEnabled), nameof(FogR), nameof(FogG), nameof(FogB),
                nameof(FogNear), nameof(FogFar), nameof(FogDensity),
                nameof(SkyboxEnabled), nameof(SkyR), nameof(SkyG), nameof(SkyB),
                nameof(ToneMapping), nameof(Exposure), nameof(EnvMapPath),
            })
        {
            OnPropertyChanged(p);
        }
    }

    private void OnEnvChanged(object? sender, PropertyChangedEventArgs e)
    {
        OnPropertyChanged(e.PropertyName);
        ApplyToNative();
    }

    /// <summary>
    /// Re-applies the complete scene environment after the native render
    /// pipeline is created or recreated. Scene attachment can happen before
    /// the Render Surface exists, so the initial best-effort push is not
    /// sufficient on its own.
    /// </summary>
    public void ApplyToNative()
    {
        if (_env == null) return;
        var p = new float[24];
        p[0] = _env.AmbientR;
        p[1] = _env.AmbientG;
        p[2] = _env.AmbientB;
        p[3] = _env.AmbientIntensity;
        p[4] = _env.FogR;
        p[5] = _env.FogG;
        p[6] = _env.FogB;
        p[7] = _env.FogNear;
        p[8] = _env.FogFar;
        p[9] = _env.FogDensity;
        p[10] = _env.FogEnabled ? 1f : 0f;
        p[11] = _env.SkyR;
        p[12] = _env.SkyG;
        p[13] = _env.SkyB;
        p[14] = _env.SkyboxEnabled ? 1f : 0f;
        p[15] = (float)(int)_env.ToneMapping;
        p[16] = _env.Exposure;
        // 17..23 reserved (left at 0).
        _lastPushOk = NativeEngineBridge.UpdateSceneEnvironment(p);
        // HDR environment + skybox toggle go through their dedicated
        // pipeline APIs (UpdateSceneEnvironment is still a stub). The sky
        // only renders when there is an env map to show.
        var hasEnv = !string.IsNullOrWhiteSpace(_env.EnvMapPath);
        if (hasEnv)
        {
            var envApplied = NativeEngineBridge.PipelineSetEnvironmentMap(_env.EnvMapPath);
            if (!envApplied && NativeEngineBridge.IsLoaded)
            {
                Logger.Warning($"Failed to apply HDR environment map: {_env.EnvMapPath}");
            }
        }
        else
        {
            // Clearing the path must also clear the native IBL bake. Previously
            // only the visible skybox was disabled, so the last HDR continued
            // lighting materials until the editor process was restarted.
            NativeEngineBridge.PipelineClearEnvironmentMap();
        }
        NativeEngineBridge.PipelineSetSkyboxEnabled(_env.SkyboxEnabled && hasEnv);
        _env.MarkDirty();
        OnPropertyChanged(nameof(StatusMessage));
    }

    public float AmbientR { get => _env?.AmbientR ?? 0; set { if (_env != null) _env.AmbientR = value; } }
    public float AmbientG { get => _env?.AmbientG ?? 0; set { if (_env != null) _env.AmbientG = value; } }
    public float AmbientB { get => _env?.AmbientB ?? 0; set { if (_env != null) _env.AmbientB = value; } }
    public float AmbientIntensity { get => _env?.AmbientIntensity ?? 1; set { if (_env != null) _env.AmbientIntensity = value; } }

    public bool FogEnabled { get => _env?.FogEnabled ?? false; set { if (_env != null) _env.FogEnabled = value; } }
    public float FogR { get => _env?.FogR ?? 0; set { if (_env != null) _env.FogR = value; } }
    public float FogG { get => _env?.FogG ?? 0; set { if (_env != null) _env.FogG = value; } }
    public float FogB { get => _env?.FogB ?? 0; set { if (_env != null) _env.FogB = value; } }
    public float FogNear { get => _env?.FogNear ?? 0; set { if (_env != null) _env.FogNear = value; } }
    public float FogFar { get => _env?.FogFar ?? 0; set { if (_env != null) _env.FogFar = value; } }
    public float FogDensity { get => _env?.FogDensity ?? 0; set { if (_env != null) _env.FogDensity = value; } }

    public bool SkyboxEnabled { get => _env?.SkyboxEnabled ?? false; set { if (_env != null) _env.SkyboxEnabled = value; } }
    public float SkyR { get => _env?.SkyR ?? 0; set { if (_env != null) _env.SkyR = value; } }
    public float SkyG { get => _env?.SkyG ?? 0; set { if (_env != null) _env.SkyG = value; } }
    public float SkyB { get => _env?.SkyB ?? 0; set { if (_env != null) _env.SkyB = value; } }

    public string EnvMapPath
    {
        get => _env?.EnvMapPath ?? string.Empty;
        set
        {
            if (_env == null) return;
            // Assigning an environment map implies the user wants to see it.
            // Auto-enable BEFORE the same-value early return: re-dropping the
            // same asset (e.g. after loading a scene saved with the skybox
            // off) must still turn the skybox on.
            if (!string.IsNullOrWhiteSpace(value) && !_env.SkyboxEnabled)
            {
                _env.SkyboxEnabled = true;
            }
            if (_env.EnvMapPath == value) return;
            _env.EnvMapPath = value;
        }
    }

    public ToneMappingMode ToneMapping
    {
        get => _env?.ToneMapping ?? ToneMappingMode.ACES;
        set { if (_env != null) _env.ToneMapping = value; }
    }

    public float Exposure { get => _env?.Exposure ?? 1; set { if (_env != null) _env.Exposure = value; } }
}
