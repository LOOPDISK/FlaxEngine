// Copyright (c) Wojciech Figat. All rights reserved.

namespace FlaxEngine
{
    partial class Camera
    {
        /// <summary>
        /// Legacy alias for <see cref="WeaponFieldOfView"/>.
        /// </summary>
        public float WeaponFOV
        {
            get => WeaponFieldOfView;
            set => WeaponFieldOfView = value;
        }
    }
}
