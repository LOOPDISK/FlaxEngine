// Copyright (c) Wojciech Figat. All rights reserved.

#if !FLAX_EDITOR
namespace FlaxEngine
{
    partial class ProfilerCPU
    {
        partial struct Event
        {
            public unsafe string Name
            {
                get
                {
                    fixed (short* name = Name0)
                        return new string((char*)name);
                }
            }
        }
    }
}
#endif
