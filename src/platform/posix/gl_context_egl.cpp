#if !defined(__EMSCRIPTEN__)
#include "gl/gl_context.h"

#include <helix/gl/egl.h>
#include <helix/gl/loader.h>

namespace helix::rhi
{
    namespace
    {
        using namespace helix::gl::egl;

        class EglContext final : public GLContext
        {
        public:
            EGLDisplay display = EGL_NO_DISPLAY;
            EGLSurface surface = EGL_NO_SURFACE;
            EGLContext context = EGL_NO_CONTEXT;
            bool es = false;

            ~EglContext() override
            {
                if (display == EGL_NO_DISPLAY)
                    return;
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (context != EGL_NO_CONTEXT)
                    eglDestroyContext(display, context);
                if (surface != EGL_NO_SURFACE)
                    eglDestroySurface(display, surface);
                eglTerminate(display);
            }

            bool MakeCurrent() override { return eglMakeCurrent(display, surface, surface, context) != 0; }
            void Swap() override { eglSwapBuffers(display, surface); }
            void SetSwapInterval(int interval) override { eglSwapInterval(display, interval); }

            void GetDrawableSize(u32& width, u32& height) override
            {
                EGLint w = 0, h = 0;
                eglQuerySurface(display, surface, EGL_WIDTH, &w);
                eglQuerySurface(display, surface, EGL_HEIGHT, &h);
                width = u32(w);
                height = u32(h);
            }

            bool IsES() const override { return es; }

            bool Init(const DeviceDesc& desc)
            {
                if (!gl::LoadWindowSystem())
                    return false;
#if defined(__ANDROID__)
                es = true;
                display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
                es = desc.backend == Backend::OpenGLES;
                display = eglGetDisplay(desc.window.display ? EGLNativeDisplayType(desc.window.display) : EGL_DEFAULT_DISPLAY);
#endif
                EGLint major = 0, minor = 0;
                if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor))
                    return false;
                bool windowed = desc.window.window != nullptr;
                const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGLint(windowed ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT), EGL_RENDERABLE_TYPE,
                                                EGLint(es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT), EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                                EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
                EGLConfig config = nullptr;
                EGLint count = 0;
                if (!eglChooseConfig(display, configAttribs, &config, 1, &count) || count == 0)
                    return false;
                if (windowed)
                    surface = eglCreateWindowSurface(display, config, EGLNativeWindowType(desc.window.window), nullptr);
                else
                {
                    const EGLint pbufferAttribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
                    surface = eglCreatePbufferSurface(display, config, pbufferAttribs);
                }
                if (surface == EGL_NO_SURFACE || !eglBindAPI(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API))
                    return false;
                const EGLint versions[][2] = {{4, 6}, {4, 5}, {4, 3}, {3, 3}};
                const EGLint esVersions[][2] = {{3, 2}, {3, 1}, {3, 0}};
                for (const auto& v : es ? Span<const EGLint[2]>(esVersions) : Span<const EGLint[2]>(versions))
                {
                    const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, v[0], EGL_CONTEXT_MINOR_VERSION, v[1],
                                              EGLint(es ? EGL_NONE : EGL_CONTEXT_OPENGL_PROFILE_MASK), EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
                    context = eglCreateContext(display, config, EGL_NO_CONTEXT, attribs);
                    if (context != EGL_NO_CONTEXT)
                        break;
                }
                if (context == EGL_NO_CONTEXT || !MakeCurrent())
                    return false;
                return gl::LoadWithCurrentContext() > 0;
            }
        };
    }

    GLContext* CreateGLContext(const DeviceDesc& desc)
    {
        auto* c = New<EglContext>(HeapAllocator());
        if (!c->Init(desc))
        {
            Delete(HeapAllocator(), c);
            return nullptr;
        }
        return c;
    }

    void DestroyGLContext(GLContext* context) { Delete(HeapAllocator(), static_cast<EglContext*>(context)); }
}
#endif
