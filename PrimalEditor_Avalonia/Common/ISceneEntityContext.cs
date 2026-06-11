using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Common;

public interface ISceneEntityContext
{
    void AddEntity(GameEntity entity);
    void RemoveEntity(GameEntity entity);
}