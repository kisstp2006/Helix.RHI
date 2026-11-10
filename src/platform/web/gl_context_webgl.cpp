#include "gl/gl_context.h"

#include <helix/gl/loader.h>

#include <emscripten/html5.h>

namespace helix::rhi
{
    namespace
    {
        class WebGLContext final : public GLContext
        {
        public:
            EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = 0;
            String selector;

            ~WebGLContext() override
            {
                if (context > 0)
                    emscripten_webgl_destroy_context(context);
            }

            bool MakeCurrent() override { return emscripten_webgl_make_context_current(context) == EMSCRIPTEN_RESULT_SUCCESS; }
            void Swap() override {}
            void SetSwapInterval(int) override {}

            void GetDrawableSize(u32& width, u32& height) override
            {
                int w = 0, h = 0;
                emscripten_get_canvas_element_size(selector.CStr(), &w, &h);
                width = u32(w);
                height = u32(h);
            }

            bool IsES() const override { return true; }

            bool Init(const DeviceDesc& desc)
            {
                selector = desc.window.window ? static_cast<const char*>(desc.window.window) : "#canvas";
                EmscriptenWebGLContextAttributes attrs;
                emscripten_webgl_init_context_attributes(&attrs);
                attrs.majorVersion = 2;
                attrs.minorVersion = 0;
                attrs.alpha = EM_FALSE;
                attrs.depth = EM_TRUE;
                attrs.stencil = EM_TRUE;
                attrs.antialias = EM_FALSE;
                attrs.preserveDrawingBuffer = EM_FALSE;
                attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
                context = emscripten_webgl_create_context(selector.CStr(), &attrs);
                if (context <= 0 || !MakeCurrent())
                    return false;
                return gl::LoadWithCurrentContext() > 0;
            }
        };
    }

    GLContext* CreateGLContext(const DeviceDesc& desc)
    {
        auto* c = New<WebGLContext>(HeapAllocator());
        if (!c->Init(desc))
        {
            Delete(HeapAllocator(), c);
            return nullptr;
        }
        return c;
    }

    void DestroyGLContext(GLContext* context) { Delete(HeapAllocator(), static_cast<WebGLContext*>(context)); }
}
