#include "gl/gl_context.h"

#include <helix/gl/loader.h>
#include <helix/gl/wgl.h>

#include <windows.h>

namespace helix::rhi
{
    namespace
    {
        namespace wgl = helix::gl::wgl;

        class WglContext final : public GLContext
        {
        public:
            HWND hwnd = nullptr;
            bool ownsWindow = false;
            HDC dc = nullptr;
            wgl::HGLRC context = nullptr;

            ~WglContext() override
            {
                if (context)
                {
                    wgl::wglMakeCurrent(nullptr, nullptr);
                    wgl::wglDeleteContext(context);
                }
                if (dc)
                    ReleaseDC(hwnd, dc);
                if (ownsWindow && hwnd)
                    DestroyWindow(hwnd);
            }

            bool MakeCurrent() override { return wgl::wglMakeCurrent(reinterpret_cast<wgl::HDC>(dc), context) != 0; }
            void Swap() override { wgl::SwapBuffers(reinterpret_cast<wgl::HDC>(dc)); }

            void SetSwapInterval(int interval) override
            {
                if (wgl::api.SwapIntervalEXT)
                    wgl::wglSwapIntervalEXT(interval);
            }

            void GetDrawableSize(u32& width, u32& height) override
            {
                RECT r{};
                GetClientRect(hwnd, &r);
                width = u32(r.right - r.left);
                height = u32(r.bottom - r.top);
            }

            bool IsES() const override { return false; }

            bool Init(const DeviceDesc& desc)
            {
                if (!gl::LoadWindowSystem())
                    return false;
                hwnd = static_cast<HWND>(desc.window.window);
                if (!hwnd)
                {
                    WNDCLASSW wc{};
                    wc.style = CS_OWNDC;
                    wc.lpfnWndProc = DefWindowProcW;
                    wc.hInstance = GetModuleHandleW(nullptr);
                    wc.lpszClassName = L"HelixGLHidden";
                    RegisterClassW(&wc);
                    hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
                    ownsWindow = true;
                }
                if (!hwnd)
                    return false;
                dc = GetDC(hwnd);
                auto hdc = reinterpret_cast<wgl::HDC>(dc);
                wgl::PIXELFORMATDESCRIPTOR pfd{};
                pfd.nSize = sizeof(pfd);
                pfd.nVersion = 1;
                pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
                pfd.cColorBits = 32;
                pfd.cAlphaBits = 8;
                pfd.cDepthBits = 24;
                pfd.cStencilBits = 8;
                int format = wgl::ChoosePixelFormat(hdc, &pfd);
                if (!format || !wgl::SetPixelFormat(hdc, format, &pfd))
                    return false;
                wgl::HGLRC legacy = wgl::wglCreateContext(hdc);
                if (!legacy || !wgl::wglMakeCurrent(hdc, legacy))
                    return false;
                wgl::Load(&gl::GetSystemProcAddress, nullptr);
                if (wgl::api.CreateContextAttribsARB)
                {
                    const int versions[][2] = {{4, 6}, {4, 5}, {4, 3}, {3, 3}};
                    for (auto& v : versions)
                    {
                        const int attribs[] = {int(wgl::WGL_CONTEXT_MAJOR_VERSION_ARB), v[0], int(wgl::WGL_CONTEXT_MINOR_VERSION_ARB), v[1],
                                               int(wgl::WGL_CONTEXT_PROFILE_MASK_ARB), int(wgl::WGL_CONTEXT_CORE_PROFILE_BIT_ARB),
                                               int(wgl::WGL_CONTEXT_FLAGS_ARB), desc.debug ? int(wgl::WGL_CONTEXT_DEBUG_BIT_ARB) : 0, 0};
                        context = wgl::wglCreateContextAttribsARB(hdc, nullptr, attribs);
                        if (context)
                            break;
                    }
                }
                if (context)
                {
                    wgl::wglMakeCurrent(hdc, context);
                    wgl::wglDeleteContext(legacy);
                }
                else
                {
                    context = legacy;
                }
                return gl::LoadWithCurrentContext() > 0;
            }
        };
    }

    GLContext* CreateGLContext(const DeviceDesc& desc)
    {
        auto* c = New<WglContext>(HeapAllocator());
        if (!c->Init(desc))
        {
            Delete(HeapAllocator(), c);
            return nullptr;
        }
        return c;
    }

    void DestroyGLContext(GLContext* context) { Delete(HeapAllocator(), static_cast<WglContext*>(context)); }
}
