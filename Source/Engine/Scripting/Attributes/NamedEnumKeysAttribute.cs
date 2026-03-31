// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEngine
{
    /// <summary>
    /// When placed on an enum type, dictionary keys using that enum will be serialized
    /// as string names instead of integers. This makes saved data resilient to enum reordering.
    /// </summary>
    [AttributeUsage(AttributeTargets.Enum)]
    public sealed class NamedEnumKeysAttribute : Attribute
    {
    }
}
