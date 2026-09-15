using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

/// <summary>
/// Particle emitter section refresh in the Inspector. Regression for the
/// "right-click add emitter shows nothing / stale empty fields" bug: the
/// section visibility and the Preset/SpawnRate/Enabled bindings must all
/// re-evaluate when the selection (re)resolves, and immediately after the
/// component is added to the already-selected entity.
/// </summary>
public sealed class PropertiesViewModelParticleTests
{
    private static GameEntity EntityWithEmitter(string name = "Plane_1") 
    {
        var entity = new GameEntity(name);
        entity.Components.Add(new Transform());
        entity.Components.Add(new ParticleEmitterComponent());
        return entity;
    }

    private static List<string> Track(PropertiesViewModel vm) 
    {
        var raised = new List<string>();
        vm.PropertyChanged += (_, e) => raised.Add(e.PropertyName ?? "");
        return raised;
    }

    [Fact]
    public void SelectingEntityWithEmitter_RaisesAllParticleBindings()
    {
        var properties = new PropertiesViewModel();
        var raised = Track(properties);

        properties.Entities = new[] { EntityWithEmitter() };

        Assert.True(properties.HasParticleComponent);
        foreach (var name in new[] { nameof(properties.Particle), nameof(properties.ParticlePresetName),
                     nameof(properties.ParticleSpawnRate), nameof(properties.ParticleEnabled) })
        {
            Assert.Contains(name, raised);
        }
        // Fresh C# defaults must surface immediately (not the pre-component
        // stale "" / 0 / false).
        Assert.Equal("Fire", properties.ParticlePresetName);
        Assert.Equal(50f, properties.ParticleSpawnRate);
        Assert.True(properties.ParticleEnabled);
    }

    [Fact]
    public void ReassigningSameSelection_StillRefreshesParticleFields()
    {
        // The add-component path re-assigns Properties.Entities with the SAME
        // array (selection didn't change) — the setter must re-notify anyway.
        var entity = EntityWithEmitter();
        var properties = new PropertiesViewModel { Entities = Array.Empty<GameEntity>() };
        var raised = Track(properties);

        properties.Entities = new[] { entity };

        Assert.Contains(nameof(properties.HasParticleComponent), raised);
        Assert.Contains(nameof(properties.ParticlePresetName), raised);
        Assert.Equal("Fire", properties.ParticlePresetName);
    }

    [Fact]
    public void DeselectingEntity_ClearsParticleSection()
    {
        var properties = new PropertiesViewModel { Entities = new[] { EntityWithEmitter() } };
        Assert.True(properties.HasParticleComponent);

        properties.Entities = Array.Empty<GameEntity>();

        Assert.False(properties.HasParticleComponent);
        Assert.Equal(string.Empty, properties.ParticlePresetName);
    }
}
