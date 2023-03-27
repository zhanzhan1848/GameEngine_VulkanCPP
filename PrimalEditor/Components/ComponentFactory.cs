using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace PrimalEditor.Components
{
    enum ComponentType
    {
        Transform,

    }
    static class ComponentFactory
    {
        private static readonly Func<GameEntity, object, Component>[] _function =
            new Func<GameEntity, object, Component>[]
            {
                (entity, data) => new Transform(entity),
            };

        public static Func<GameEntity, object, Component> GetCreationFunction(ComponentType componentType)
        {
            System.Diagnostics.Debug.Assert((int)componentType < _function.Length);
            return _function[(int)componentType];
        }
    }
}
