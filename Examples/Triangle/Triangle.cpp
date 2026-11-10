#include <Helix/Core/Logging/Log.h>
#include <Helix/Platform/Time/Time.h>
#include <Helix/Platform/Window/Window.h>
#include <Helix/RHI/Device/Device.h>

#include <cmath>
#include <cstring>

using namespace helix;
using namespace helix::rhi;

namespace
{
    LogChannel DemoLog{"Triangle"};

    const char* kVertex = "layout(location = 0) in vec2 p; layout(location = 1) in vec3 c; out vec3 vc;"
                          "layout(std140) uniform Frame { vec4 rotation; };"
                          "void main() { vec2 r = vec2(p.x * rotation.x - p.y * rotation.y, p.x * rotation.y + p.y * rotation.x);"
                          " gl_Position = vec4(r * vec2(rotation.z, 1.0), 0.5, 1.0); vc = c; }";
    const char* kFragment = "in vec3 vc; out vec4 o; void main() { o = vec4(vc, 1.0); }";

    struct App
    {
        platform::Window* window = nullptr;
        Device* device = nullptr;
        PipelineHandle pipeline;
        BufferHandle vertices;
        platform::Stopwatch clock;
        Array<platform::Event> events;
        f64 autoClose = 0;
        u64 frames = 0;
    };

    String Source(const DeviceCaps& caps, const char* body)
    {
        bool es = caps.backend != Backend::OpenGL;
        return helix::Format("{}{}", es ? "#version 300 es\nprecision highp float;\n" : "#version 330 core\n", body);
    }

    bool Frame(void* user)
    {
        auto& app = *static_cast<App*>(user);
        app.events.Clear();
        app.window->PollEvents(app.events);
        i32 w = 1, h = 1;
        app.window->GetFramebufferSize(w, h);
        app.device->Resize(u32(w), u32(h));
        app.device->BeginFrame();
        f32 t = f32(app.clock.ElapsedSeconds());
        TransientAllocation frame = app.device->AllocateTransient(16);
        f32 data[4] = {std::cos(t), std::sin(t), f32(h) / f32(w > 0 ? w : 1), 0};
        std::memcpy(frame.data, data, sizeof(data));
        CommandList cl;
        RenderPassDesc rp;
        rp.colors[0].clearColor[0] = 0.08f;
        rp.colors[0].clearColor[1] = 0.08f;
        rp.colors[0].clearColor[2] = 0.1f;
        cl.BeginRenderPass(rp);
        cl.SetPipeline(app.pipeline);
        cl.SetVertexBuffer(0, app.vertices);
        cl.SetUniformBuffer(0, frame);
        cl.Draw(3);
        cl.EndRenderPass();
        app.device->Submit(cl);
        app.device->EndFrame();
        app.device->Present();
        if (++app.frames == 120)
            HX_LOG_INFO(DemoLog, "120 frames in {:.2} s", app.clock.ElapsedSeconds());
        if (app.autoClose > 0 && app.clock.ElapsedSeconds() > app.autoClose)
            return false;
        return !app.window->CloseRequested();
    }
}

int main(int argc, char** argv)
{
    static App app;
    for (int i = 1; i < argc; i++)
        if (StringView(argv[i]) == "--auto-close")
            app.autoClose = 3.0;
#if HX_PLATFORM_WEB
    Backend backend = Backend::WebGL2;
#elif HX_PLATFORM_ANDROID
    Backend backend = Backend::OpenGLES;
#else
    Backend backend = Backend::OpenGL;
#endif
    app.window = platform::Window::Create({.title = "Helix RHI triangle", .width = 800, .height = 600});
    if (!app.window)
        return 1;
    app.device = Device::Create({.backend = backend, .window = app.window->GetNativeHandle()});
    if (!app.device)
    {
        HX_LOG_ERROR(DemoLog, "no device");
        return 1;
    }
    const DeviceCaps& caps = app.device->Caps();
    HX_LOG_INFO(DemoLog, "{}.{} on {}", caps.apiMajor, caps.apiMinor, caps.renderer);
    const ShaderBinding bindings[] = {{"Frame", BindingKind::UniformBuffer, 0}};
    String vs = Source(caps, kVertex), fs = Source(caps, kFragment);
    ShaderHandle v = app.device->CreateShader({.stage = ShaderStage::Vertex, .source = vs, .bindings = bindings, .bindingCount = 1});
    ShaderHandle f = app.device->CreateShader({.stage = ShaderStage::Fragment, .source = fs});
    PipelineDesc pd;
    pd.vertexShader = v;
    pd.fragmentShader = f;
    pd.vertexBuffers[0] = {20, StepMode::Vertex};
    pd.vertexBufferCount = 1;
    pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
    pd.attributes[1] = {1, 0, VertexFormat::Float3, 8};
    pd.attributeCount = 2;
    app.pipeline = app.device->CreatePipeline(pd);
    const f32 verts[] = {0.0f, 0.7f, 1, 0.3f, 0.2f, -0.6f, -0.5f, 0.2f, 1, 0.3f, 0.6f, -0.5f, 0.2f, 0.4f, 1};
    app.vertices = app.device->CreateBuffer({.size = sizeof(verts), .usage = BufferVertex, .initialData = verts});
    platform::RunMainLoop(&Frame, &app);
#if !HX_PLATFORM_WEB
    Device::Destroy(app.device);
    platform::Window::Destroy(app.window);
#endif
    return 0;
}
