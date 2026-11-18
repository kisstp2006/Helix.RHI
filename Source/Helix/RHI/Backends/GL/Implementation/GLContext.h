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

        // Surfaces of other windows the context draws to (made current on them); null where a platform has none.
        virtual void* CreateSurface(platform::NativeWindowHandle) { return nullptr; }
        virtual void DestroySurface(void*) {}
        // The context current on a surface (null: its own window's).
        virtual bool MakeCurrentOn(void* surface) { return surface ? false : MakeCurrent(); }
        virtual void SwapSurface(void* surface)
        {
            if (!surface)
                Swap();
        }
        virtual void GetSurfaceSize(void* surface, u32& width, u32& height)
        {
            width = height = 0;
            if (!surface)
                GetDrawableSize(width, height);
        }
    };

    // Creates the platform context (WGL, EGL or WebGL) and loads the GL entry points.
    GLContext* CreateGLContext(const DeviceDesc& desc);
    void DestroyGLContext(GLContext* context);
}
