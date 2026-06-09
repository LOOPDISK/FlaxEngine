// Copyright (c) Wojciech Figat. All rights reserved.
#pragma once

// [RenderStall] DIAGNOSTIC - remove after PSO/shader cold-compile stall is pinned.
// Logs render-thread D3D11 calls that block longer than the threshold (first-use shader
// object / input layout creation, driver draw-time compile). Throttled. No-op in release.
#if GRAPHICS_API_DIRECTX11

#include "Engine/Core/Types/String.h"
#if !BUILD_RELEASE
#include "Engine/Platform/Platform.h"
#include "Engine/Core/Log.h"
#endif

namespace RenderStallProbeDX11
{
    // us = elapsed since t0; logs if over threshold. Global throttle keeps a burst bounded.
    inline void Report(const char* what, const StringAnsi& name, double t0)
    {
#if !BUILD_RELEASE
        const double us = (Platform::GetTimeSeconds() - t0) * 1e6;
        if (us < 1000.0)
            return;
        static double lastLog = 0.0;
        const double now = Platform::GetTimeSeconds();
        if (now - lastLog < 0.005)
            return;
        lastLog = now;
        LOG(Info, "[RenderStall] {0} us={1} {2}", String(what), (int32)us, name.HasChars() ? String(name.Get()) : String::Empty);
#endif
    }
}

#endif
