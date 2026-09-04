using PrimalEditor_Avalonia.Components;

namespace PrimalEditor_Avalonia.Common;

public interface ISceneEntityContext
{
    void AddEntity(GameEntity entity);
    void RemoveEntity(GameEntity entity);

    /// <summary>Move an entity under a new parent (null = scene root).
    /// Returns false for self/descendant targets or no-op moves.</summary>
    bool ReparentEntity(GameEntity entity, GameEntity? newParent);
}