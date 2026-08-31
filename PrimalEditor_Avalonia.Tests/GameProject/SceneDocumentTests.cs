using System;
using System.IO;
using System.Text.Json;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Documents;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.GameProject;

public sealed class SceneDocumentTests
{
    [Fact]
    public void SceneDocument_SerializesSceneGuidNameAndEntities()
    {
        // Arrange: Create a scene with GUID, name, and entities
        var sceneGuid = new SceneGuid(Guid.NewGuid());
        var scene = new Scene("TestScene")
        {
            Guid = sceneGuid
        };

        var entity1 = new GameEntity("Entity1");
        entity1.Components.Add(new Transform { PositionX = 1.0f, PositionY = 2.0f, PositionZ = 3.0f });

        var entity2 = new GameEntity("Entity2");
        entity2.Components.Add(new Transform { ScaleX = 2.0f, ScaleY = 2.0f, ScaleZ = 2.0f });

        scene.Entities.Add(entity1);
        scene.Entities.Add(entity2);

        // Act: Convert to document and serialize
        var document = Serializer.ToSceneDocument(scene);
        var json = JsonSerializer.Serialize(document, SceneDocument.JsonOptions);
        var parsed = JsonDocument.Parse(json);

        // Assert: Verify document structure
        Assert.Equal(sceneGuid.Value.ToString("D").ToLowerInvariant(), parsed.RootElement.GetProperty("$id").GetString());
        Assert.Equal("TestScene", parsed.RootElement.GetProperty("Name").GetString());
        Assert.Equal(2, parsed.RootElement.GetProperty("Entities").GetArrayLength());
    }

    [Fact]
    public void SceneDocument_EntityHasStableGuidPreservedAcrossRoundTrip()
    {
        // Arrange: Create entity with GUID
        var entityGuid = new EntityGuid(Guid.NewGuid());
        var entity = new GameEntity("PersistentEntity")
        {
            EntityGuid = entityGuid
        };
        entity.Components.Add(new Transform { PositionX = 10.0f });

        var scene = new Scene("PersistenceTest") { Entities = { entity } };

        // Act: Round-trip through document
        var document = Serializer.ToSceneDocument(scene);
        var loadedEntity = document.Entities.FirstOrDefault();

        // Assert: GUID preserved
        Assert.NotNull(loadedEntity);
        Assert.Equal(entityGuid, loadedEntity.EntityGuid);
    }

    [Fact]
    public void SceneDocument_FlattensHierarchyIntoCanonicalParentGuidArray()
    {
        // Arrange: Create hierarchy
        var parent = new GameEntity("Parent")
        {
            EntityGuid = new EntityGuid(Guid.NewGuid())
        };
        parent.Components.Add(new Transform { PositionX = 1.0f });

        var child = new GameEntity("Child")
        {
            EntityGuid = new EntityGuid(Guid.NewGuid()),
            Parent = parent
        };
        child.Components.Add(new Transform { PositionX = 2.0f });

        var grandchild = new GameEntity("Grandchild")
        {
            EntityGuid = new EntityGuid(Guid.NewGuid()),
            Parent = child
        };
        grandchild.Components.Add(new Transform { PositionX = 3.0f });

        // Build runtime hierarchy
        parent.Children.Add(child);
        child.Children.Add(grandchild);

        var scene = new Scene("HierarchyTest") { Entities = { parent } };

        // Act: Convert to document
        var document = Serializer.ToSceneDocument(scene);

        // Assert: All entities in flat array with canonical parent GUIDs
        Assert.Equal(3, document.Entities.Count);

        var parentDto = document.Entities.FirstOrDefault(e => e.EntityGuid == parent.EntityGuid);
        var childDto = document.Entities.FirstOrDefault(e => e.EntityGuid == child.EntityGuid);
        var grandchildDto = document.Entities.FirstOrDefault(e => e.EntityGuid == grandchild.EntityGuid);

        Assert.NotNull(parentDto);
        Assert.NotNull(childDto);
        Assert.NotNull(grandchildDto);

        // Parent has no parent (root)
        Assert.Null(parentDto.ParentEntityGuid);
        // Child's parent is parent entity
        Assert.Equal(parent.EntityGuid, childDto.ParentEntityGuid);
        // Grandchild's parent is child entity
        Assert.Equal(child.EntityGuid, grandchildDto.ParentEntityGuid);
    }

    [Fact]
    public void SceneDocument_PersistsTransformFieldsOnly()
    {
        // Arrange: Create entity with Transform
        var entity = new GameEntity("TransformEntity")
        {
            EntityGuid = new EntityGuid(Guid.NewGuid())
        };
        var transform = new Transform
        {
            PositionX = 5.0f,
            PositionY = 10.0f,
            PositionZ = 15.0f,
            RotationX = 1.0f,
            RotationY = 2.0f,
            RotationZ = 3.0f,
            ScaleX = 2.0f,
            ScaleY = 3.0f,
            ScaleZ = 4.0f
        };
        entity.Components.Add(transform);

        var scene = new Scene("TransformTest") { Entities = { entity } };

        // Act: Convert to document and serialize
        var document = Serializer.ToSceneDocument(scene);
        var json = JsonSerializer.Serialize(document, SceneDocument.JsonOptions);
        var parsed = JsonDocument.Parse(json);

        // Assert: Transform fields persisted, runtime fields omitted
        var entityJson = parsed.RootElement.GetProperty("Entities")[0];
        var components = entityJson.GetProperty("Components");

        Assert.Single(components.EnumerateArray());

        var componentDoc = components[0];
        Assert.Equal("transform", componentDoc.GetProperty("$type").GetString());
        var transformJson = componentDoc.GetProperty("Data");
        Assert.Equal(5.0f, transformJson.GetProperty("PositionX").GetSingle());
        Assert.Equal(10.0f, transformJson.GetProperty("PositionY").GetSingle());
        Assert.Equal(10.0f, transformJson.GetProperty("PositionY").GetSingle());
        Assert.Equal(15.0f, transformJson.GetProperty("PositionZ").GetSingle());
        Assert.Equal(1.0f, transformJson.GetProperty("RotationX").GetSingle());
        Assert.Equal(2.0f, transformJson.GetProperty("RotationY").GetSingle());
        Assert.Equal(3.0f, transformJson.GetProperty("RotationZ").GetSingle());
        Assert.Equal(2.0f, transformJson.GetProperty("ScaleX").GetSingle());
        Assert.Equal(3.0f, transformJson.GetProperty("ScaleY").GetSingle());
        Assert.Equal(4.0f, transformJson.GetProperty("ScaleZ").GetSingle());

        // Runtime fields should NOT be in JSON
        var jsonText = json;
        Assert.DoesNotContain("IsActive", jsonText, StringComparison.Ordinal);
        Assert.DoesNotContain("ParentScene", jsonText, StringComparison.Ordinal);
        Assert.DoesNotContain("\"Parent\":", jsonText, StringComparison.Ordinal);
        Assert.DoesNotContain("Children", jsonText, StringComparison.Ordinal);
        Assert.DoesNotContain("AssetPath", jsonText, StringComparison.Ordinal);
    }

    [Fact]
    public void SceneDocument_PolymorphicComponentsUseTypeDiscriminator()
    {
        // Arrange: Create entity with Transform component
        var entity = new GameEntity("PolymorphicTest")
        {
            EntityGuid = new EntityGuid(Guid.NewGuid())
        };
        entity.Components.Add(new Transform { PositionX = 1.0f });

        var scene = new Scene("PolymorphicTest") { Entities = { entity } };

        // Act: Serialize to JSON
        var document = Serializer.ToSceneDocument(scene);
        var json = JsonSerializer.Serialize(document, SceneDocument.JsonOptions);
        var parsed = JsonDocument.Parse(json);

        // Assert: Components have $type discriminator
        var components = parsed.RootElement.GetProperty("Entities")[0].GetProperty("Components");
        Assert.Single(components.EnumerateArray());

        var component = components[0];
        Assert.True(component.TryGetProperty("$type", out var typeProp));
        Assert.Equal("transform", typeProp.GetString());
    }

    [Fact]
    public void LoadScene_ReconstructsHierarchyFromFlatArray()
    {
        // Arrange: Create scene document with hierarchy
        var parentGuid = new EntityGuid(Guid.NewGuid());
        var childGuid = new EntityGuid(Guid.NewGuid());

        var document = new SceneDocument
        {
            SceneGuid = new SceneGuid(Guid.NewGuid()),
            Name = "HierarchyReconstructionTest",
            Entities = new System.Collections.Generic.List<EntityDocument>
            {
                new EntityDocument
                {
                    EntityGuid = parentGuid,
                    Name = "Parent",
                    IsEnabled = true,
                    ParentEntityGuid = null,
                    Components = new System.Collections.Generic.List<ComponentDocument>
                    {
                        new ComponentDocument { Type = "transform", Data = JsonSerializer.SerializeToElement(new { PositionX = 1.0f }) }
                    }
                },
                new EntityDocument
                {
                    EntityGuid = childGuid,
                    Name = "Child",
                    IsEnabled = true,
                    ParentEntityGuid = parentGuid,
                    Components = new System.Collections.Generic.List<ComponentDocument>
                    {
                        new ComponentDocument { Type = "transform", Data = JsonSerializer.SerializeToElement(new { PositionX = 2.0f }) }
                    }
                }
            }
        };

        // Act: Load scene
        var scene = Serializer.FromSceneDocument(document);

        // Assert: Hierarchy reconstructed. Only root entities sit on
        // scene.Entities; children attach via parent.Children.
        Assert.Single(scene.Entities);

        var parent = scene.Entities.FirstOrDefault(e => e.EntityGuid == parentGuid);
        Assert.NotNull(parent);

        var child = parent!.Children.FirstOrDefault(e => e.EntityGuid == childGuid);
        Assert.NotNull(child);

        Assert.Null(parent.Parent);
        Assert.Equal(parent, child!.Parent);
        Assert.Single(parent.Children);
        Assert.Equal(child, parent.Children[0]);
        Assert.Empty(child.Children);
        Assert.Equal(scene, parent.ParentScene);
        Assert.Equal(scene, child.ParentScene);
    }

    [Fact]
    public void LoadScene_RequiresUniqueEntityGuids()
    {
        // Arrange: Create scene document with duplicate GUIDs
        var duplicateGuid = new EntityGuid(Guid.NewGuid());
        var document = new SceneDocument
        {
            SceneGuid = new SceneGuid(Guid.NewGuid()),
            Name = "DuplicateGuidTest",
            Entities = new System.Collections.Generic.List<EntityDocument>
            {
                new EntityDocument { EntityGuid = duplicateGuid, Name = "Entity1", IsEnabled = true },
                new EntityDocument { EntityGuid = duplicateGuid, Name = "Entity2", IsEnabled = true }
            }
        };

        // Act & Assert: Should throw
        Assert.Throws<InvalidOperationException>(() => Serializer.FromSceneDocument(document));
    }

    [Fact]
    public void LoadScene_DetectsCyclesInParentReferences()
    {
        // Arrange: Create cycle: A -> B -> C -> A
        var guidA = new EntityGuid(Guid.NewGuid());
        var guidB = new EntityGuid(Guid.NewGuid());
        var guidC = new EntityGuid(Guid.NewGuid());

        var document = new SceneDocument
        {
            SceneGuid = new SceneGuid(Guid.NewGuid()),
            Name = "CycleTest",
            Entities = new System.Collections.Generic.List<EntityDocument>
            {
                new EntityDocument { EntityGuid = guidA, Name = "A", IsEnabled = true, ParentEntityGuid = guidC },
                new EntityDocument { EntityGuid = guidB, Name = "B", IsEnabled = true, ParentEntityGuid = guidA },
                new EntityDocument { EntityGuid = guidC, Name = "C", IsEnabled = true, ParentEntityGuid = guidB }
            }
        };

        // Act & Assert: Should throw
        Assert.Throws<InvalidOperationException>(() => Serializer.FromSceneDocument(document));
    }

    [Fact]
    public void LoadScene_ThrowsOnUnknownComponentType()
    {
        // Arrange: Create scene with unknown component type
        var document = new SceneDocument
        {
            SceneGuid = new SceneGuid(Guid.NewGuid()),
            Name = "UnknownComponentTest",
            Entities = new System.Collections.Generic.List<EntityDocument>
            {
                new EntityDocument
                {
                    EntityGuid = new EntityGuid(Guid.NewGuid()),
                    Name = "Entity",
                    IsEnabled = true,
                    Components = new System.Collections.Generic.List<ComponentDocument>
                    {
                        new ComponentDocument { Type = "unknown_type", Data = JsonSerializer.SerializeToElement(new { SomeField = 42 }) }
                    }
                }
            }
        };

        // Act & Assert: Should throw with useful error
        var ex = Assert.Throws<NotSupportedException>(() => Serializer.FromSceneDocument(document));
        Assert.Contains("unknown_type", ex.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("component", ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void SceneDocument_PreservesEnabledState()
    {
        // Arrange: Create entities with different enabled states
        var enabledEntity = new GameEntity("Enabled")
        {
            EntityGuid = new EntityGuid(Guid.NewGuid()),
            IsEnabled = true
        };
        var disabledEntity = new GameEntity("Disabled")
        {
            EntityGuid = new EntityGuid(Guid.NewGuid()),
            IsEnabled = false
        };

        var scene = new Scene("EnabledTest")
        {
            Entities = { enabledEntity, disabledEntity }
        };

        // Act: Round-trip
        var document = Serializer.ToSceneDocument(scene);
        var loadedScene = Serializer.FromSceneDocument(document);

        // Assert: Enabled states preserved
        var loadedEnabled = loadedScene.Entities.FirstOrDefault(e => e.EntityGuid == enabledEntity.EntityGuid);
        var loadedDisabled = loadedScene.Entities.FirstOrDefault(e => e.EntityGuid == disabledEntity.EntityGuid);

        Assert.NotNull(loadedEnabled);
        Assert.NotNull(loadedDisabled);
        Assert.True(loadedEnabled.IsEnabled);
        Assert.False(loadedDisabled.IsEnabled);
    }

    [Fact]
    public void SceneDocument_PreservesBuiltinMeshKind()
    {
        var entity = new GameEntity("Plane");
        entity.Components.Add(new Transform());
        entity.Components.Add(new MeshRendererComponent { BuiltinMesh = BuiltinMeshKind.Plane });
        var scene = new Scene("Builtins") { Entities = { entity } };

        var loaded = Serializer.FromSceneDocument(Serializer.ToSceneDocument(scene));

        Assert.Equal(
            BuiltinMeshKind.Plane,
            loaded.Entities.Single().GetComponent<MeshRendererComponent>()!.BuiltinMesh);
    }
}
