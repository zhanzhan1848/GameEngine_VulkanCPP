using System;
using System.Text.Json;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;

var sceneGuid = new SceneGuid(Guid.NewGuid());
var scene = new Scene("TestScene")
{
    Guid = sceneGuid
};

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

scene.Entities.Add(entity);

var document = Serializer.ToSceneDocument(scene);
var json = JsonSerializer.Serialize(document, SceneDocument.JsonOptions);
Console.WriteLine(json);