// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEngine
{
    /// <summary>
    /// When placed on an enum type, values of that enum will be serialized as string names
    /// instead of integers. This makes saved data resilient to enum reordering.
    /// Deserialization accepts both string names and integer values for backward compatibility.
    /// </summary>
    [AttributeUsage(AttributeTargets.Enum)]
    public sealed class NamedEnumAttribute : Attribute
    {
    }
}
