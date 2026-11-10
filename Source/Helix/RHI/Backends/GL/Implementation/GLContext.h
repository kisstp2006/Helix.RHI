#pragma once

#include <Helix/RHI/Device/Device.h>

namespace helix::rhi
{
    class GLContext
    {
    public:
        virtual ~GLContext() = default;
        virtual bool MakeCurrent() = 0;
        virtual void Swap() = 0;
        virtual void SetSwapInterval(int interval) = 0;
        virtual void GetDrawableSize(u32& width, u32& height) = 0;
        virtual bool IsES() const = 0;
    };

    // Creates the platform context (WGL, EGL or WebGL) and loads the GL entry points.
    GLContext* CreateGLContext(const DeviceDesc& desc);
    void DestroyGLContext(GLContext* context);
}
