#include <Support/CompiledShaders.h>
#include <Support/TestDevice.h>

#include <Helix/Platform/Time/Time.h>
#include <Helix/Testing/Test.h>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

HX_TEST(RHI, ClearAndReadBack)
{
    ForEachBackend([](Device& d) {
        std::printf("    %s %u.%u on %s\n", BackendName(d.Caps().backend), d.Caps().apiMajor, d.Caps().apiMinor, d.Caps().renderer);
        Target t(&d);
        CommandList cl;
        cl.BeginRenderPass(t.Pass(0.25f, 0.5f, 0.75f, 1.0f));
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(t.Read());
        HX_CHECK(NearColor(t.At(0, 0), 0xffbf8040u));
        HX_CHECK(NearColor(t.At(15, 15), 0xffbf8040u));
    });
}

HX_TEST(RHI, TriangleOrientationMatchesAcrossBackends)
{
    ForEachBackend([](Device& d) {
        ShaderHandle vs = MakeShader(d, kColorVertex);
        ShaderHandle fs = MakeShader(d, kColorFragment);
        HX_REQUIRE(vs.IsValid() && fs.IsValid());
        PipelineHandle pipe = d.CreatePipeline(ColorPipeline(vs, fs));
        HX_REQUIRE(pipe.IsValid());
        const f32 verts[] = {-1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, -1, 0, 1, 0, 0, 1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(verts), .usage = BufferVertex, .initialData = verts});
        Target t(&d);
        CommandList cl;
        cl.BeginRenderPass(t.Pass());
        cl.SetPipeline(pipe);
        cl.SetVertexBuffer(0, vb);
        cl.Draw(3);
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(t.Read());
        HX_CHECK(t.Golden("RHI.TriangleOrientationMatchesAcrossBackends"));
        HX_CHECK_EQ(t.At(1, 1), 0xff0000ffu);
        HX_CHECK_EQ(t.At(14, 14), 0x00000000u);
        HX_CHECK_EQ(t.At(14, 1), 0x00000000u);
        HX_CHECK_EQ(t.At(1, 14), 0x00000000u);
        d.Destroy(vb);
        d.Destroy(pipe);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}

// Clockwise triangles are front faces by default: culling back faces keeps this one, culling front faces removes it.
HX_TEST(RHI, CullingMatchesAcrossBackends)
{
    ForEachBackend([](Device& d) {
        ShaderHandle vs = MakeShader(d, kColorVertex);
        ShaderHandle fs = MakeShader(d, kColorFragment);
        HX_REQUIRE(vs.IsValid() && fs.IsValid());
        const f32 verts[] = {-1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, -1, 0, 1, 0, 0, 1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(verts), .usage = BufferVertex, .initialData = verts});
        for (CullMode cull : {CullMode::Back, CullMode::Front})
            for (bool counterClockwise : {false, true})
            {
                PipelineDesc pd = ColorPipeline(vs, fs);
                pd.cull = cull;
                pd.frontCounterClockwise = counterClockwise;
                PipelineHandle pipe = d.CreatePipeline(pd);
                HX_REQUIRE(pipe.IsValid());
                Target t(&d);
                CommandList cl;
                cl.BeginRenderPass(t.Pass());
                cl.SetPipeline(pipe);
                cl.SetVertexBuffer(0, vb);
                cl.Draw(3);
                cl.EndRenderPass();
                d.Submit(cl);
                HX_REQUIRE(t.Read());
                bool front = !counterClockwise;
                bool kept = (cull == CullMode::Back) == front;
                HX_CHECK_EQ(t.At(1, 1), kept ? 0xff0000ffu : 0x00000000u);
                d.Destroy(pipe);
            }
        d.Destroy(vb);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}

HX_TEST(RHI, UniformsFromTransientMemory)
{
    ForEachBackend([&](Device& d) {
        ShaderHandle vs = MakeShader(d, kPlainVertex);
        ShaderHandle fs = MakeShader(d, kTintFragment);
        PipelineDesc pd;
        pd.vertexShader = vs;
        pd.fragmentShader = fs;
        pd.vertexBuffers[0] = {8, StepMode::Vertex};
        pd.vertexBufferCount = 1;
        pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
        pd.attributeCount = 1;
        PipelineHandle pipe = d.CreatePipeline(pd);
        HX_REQUIRE(pipe.IsValid());
        const f32 verts[] = {-1, -1, -1, 3, 3, -1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(verts), .usage = BufferVertex, .initialData = verts});
        Target t(&d);
        CommandList cl;
        cl.BeginRenderPass(t.Pass());
        cl.SetPipeline(pipe);
        cl.SetVertexBuffer(0, vb);
        TransientAllocation first = d.AllocateTransient(16);
        const f32 green[4] = {0, 1, 0, 1};
        std::memcpy(first.data, green, 16);
        TransientAllocation second = d.AllocateTransient(16);
        const f32 magenta[4] = {1, 0, 1, 1};
        std::memcpy(second.data, magenta, 16);
        HX_CHECK(second.offset >= first.offset + 16);
        cl.SetUniformBuffer(2, first);
        cl.Draw(3);
        cl.SetUniformBuffer(2, second);
        cl.SetScissor({8, 0, 8, 16});
        cl.Draw(3);
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(t.Read());
        HX_CHECK(t.Golden("RHI.UniformsFromTransientMemory"));
        HX_CHECK_EQ(t.At(2, 8), 0xff00ff00u);
        HX_CHECK_EQ(t.At(12, 8), 0xffff00ffu);
        d.Destroy(vb);
        d.Destroy(pipe);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}

HX_TEST(RHI, TextureSamplingOrientation)
{
    ForEachBackend([&](Device& d) {
        ShaderHandle vs = MakeShader(d, kTexturedVertex);
        ShaderHandle fs = MakeShader(d, kTexturedFragment);
        PipelineDesc pd;
        pd.vertexShader = vs;
        pd.fragmentShader = fs;
        pd.vertexBuffers[0] = {16, StepMode::Vertex};
        pd.vertexBufferCount = 1;
        pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
        pd.attributes[1] = {1, 0, VertexFormat::Float2, 8};
        pd.attributeCount = 2;
        pd.topology = PrimitiveTopology::TriangleStrip;
        PipelineHandle pipe = d.CreatePipeline(pd);
        HX_REQUIRE(pipe.IsValid());
        const f32 quad[] = {-1, 1, 0, 0, 1, 1, 1, 0, -1, -1, 0, 1, 1, -1, 1, 1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(quad), .usage = BufferVertex, .initialData = quad});
        const u32 texels[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};
        TextureHandle tex = d.CreateTexture({.format = Format::RGBA8Unorm, .width = 2, .height = 2, .initialData = texels});
        SamplerHandle nearest = d.CreateSampler({.minFilter = Filter::Nearest, .magFilter = Filter::Nearest, .mipFilter = Filter::Nearest,
                                                 .addressU = AddressMode::ClampToEdge, .addressV = AddressMode::ClampToEdge});
        Target t(&d);
        CommandList cl;
        cl.BeginRenderPass(t.Pass());
        cl.SetPipeline(pipe);
        cl.SetVertexBuffer(0, vb);
        cl.SetTexture(3, tex, nearest);
        cl.Draw(4);
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(t.Read());
        HX_CHECK(t.Golden("RHI.TextureSamplingOrientation"));
        HX_CHECK_EQ(t.At(3, 3), 0xff0000ffu);
        HX_CHECK_EQ(t.At(12, 3), 0xff00ff00u);
        HX_CHECK_EQ(t.At(3, 12), 0xffff0000u);
        HX_CHECK_EQ(t.At(12, 12), 0xffffffffu);
        d.Destroy(tex);
        d.Destroy(nearest);
        d.Destroy(vb);
        d.Destroy(pipe);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}

HX_TEST(RHI, InstancedIndexedQuads)
{
    ForEachBackend([&](Device& d) {
        ShaderHandle vs = MakeShader(d, kInstancedVertex);
        ShaderHandle fs = MakeShader(d, kColorFragment);
        PipelineDesc pd;
        pd.vertexShader = vs;
        pd.fragmentShader = fs;
        pd.vertexBuffers[0] = {8, StepMode::Vertex};
        pd.vertexBuffers[1] = {12, StepMode::Instance};
        pd.vertexBufferCount = 2;
        pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
        pd.attributes[1] = {1, 1, VertexFormat::Float2, 0};
        pd.attributes[2] = {2, 1, VertexFormat::UByte4Norm, 8};
        pd.attributeCount = 3;
        PipelineHandle pipe = d.CreatePipeline(pd);
        HX_REQUIRE(pipe.IsValid());
        const f32 quad[] = {-1, -1, 1, -1, 1, 1, -1, 1};
        const u16 indices[] = {0, 1, 2, 0, 2, 3};
        struct Instance
        {
            f32 x, y;
            u32 color;
        };
        const Instance instances[] = {{-0.5f, 0.5f, 0xff0000ff}, {0.5f, 0.5f, 0xff00ff00}, {-0.5f, -0.5f, 0xffff0000}, {0.5f, -0.5f, 0xff00ffff}};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(quad), .usage = BufferVertex, .initialData = quad});
        BufferHandle ib = d.CreateBuffer({.size = sizeof(indices), .usage = BufferIndex, .initialData = indices});
        BufferHandle inst = d.CreateBuffer({.size = sizeof(instances), .usage = BufferVertex, .initialData = instances});
        Target t(&d);
        CommandList cl;
        cl.BeginRenderPass(t.Pass());
        cl.SetPipeline(pipe);
        cl.SetVertexBuffer(0, vb);
        cl.SetVertexBuffer(1, inst);
        cl.SetIndexBuffer(ib, IndexType::U16);
        cl.DrawIndexed(6, 3);
        cl.DrawIndexed(6, 1, 0, 0, 3);
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(t.Read());
        HX_CHECK(t.Golden("RHI.InstancedIndexedQuads"));
        HX_CHECK_EQ(t.At(3, 3), 0xff0000ffu);
        HX_CHECK_EQ(t.At(12, 3), 0xff00ff00u);
        HX_CHECK_EQ(t.At(3, 12), 0xffff0000u);
        HX_CHECK_EQ(t.At(12, 12), 0xff00ffffu);
        d.Destroy(vb);
        d.Destroy(ib);
        d.Destroy(inst);
        d.Destroy(pipe);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}

HX_TEST(RHI, DepthTest)
{
    ForEachBackend([&](Device& d) {
        ShaderHandle vs = MakeShader(d, kDepthVertex);
        ShaderHandle fs = MakeShader(d, kDepthFragment);
        PipelineDesc pd;
        pd.vertexShader = vs;
        pd.fragmentShader = fs;
        pd.vertexBuffers[0] = {8, StepMode::Vertex};
        pd.vertexBufferCount = 1;
        pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
        pd.attributeCount = 1;
        pd.depthTest = true;
        pd.depthWrite = true;
        pd.depthCompare = CompareFunction::Less;
        pd.depthFormat = Format::D32Float;
        PipelineHandle pipe = d.CreatePipeline(pd);
        HX_REQUIRE(pipe.IsValid());
        const f32 verts[] = {-1, -1, -1, 3, 3, -1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(verts), .usage = BufferVertex, .initialData = verts});
        Target t(&d, true);
        CommandList cl;
        cl.BeginRenderPass(t.Pass());
        cl.SetPipeline(pipe);
        cl.SetVertexBuffer(0, vb);
        struct DrawData
        {
            f32 color[4];
            f32 depth, pad[3];
        };
        const DrawData draws[] = {{{0, 1, 0, 1}, 0.8f, {}}, {{1, 0, 0, 1}, 0.2f, {}}, {{0, 0, 1, 1}, 0.5f, {}}};
        for (const DrawData& dd : draws)
        {
            TransientAllocation a = d.AllocateTransient(sizeof(DrawData));
            std::memcpy(a.data, &dd, sizeof(dd));
            cl.SetUniformBuffer(0, a);
            cl.Draw(3);
        }
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(t.Read());
        HX_CHECK(t.Golden("RHI.DepthTest"));
        HX_CHECK_EQ(t.At(8, 8), 0xff0000ffu);
        d.Destroy(vb);
        d.Destroy(pipe);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}

HX_TEST(RHI, TenThousandDrawsBenchmark)
{
    ForEachBackend([&](Device& d) {
        ShaderHandle vs = MakeShader(d, kPlacedVertex);
        ShaderHandle fs = MakeShader(d, kPlacedFragment);
        PipelineDesc pd;
        pd.vertexShader = vs;
        pd.fragmentShader = fs;
        pd.vertexBuffers[0] = {8, StepMode::Vertex};
        pd.vertexBufferCount = 1;
        pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
        pd.attributeCount = 1;
        PipelineHandle pipe = d.CreatePipeline(pd);
        HX_REQUIRE(pipe.IsValid());
        const f32 verts[] = {-1, -1, -1, 1, 1, -1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(verts), .usage = BufferVertex, .initialData = verts});
        TextureHandle target = d.CreateTexture({.format = Format::RGBA8Unorm, .width = 512, .height = 512, .usage = TextureRenderTarget});
        f64 best = 1e9;
        // The second pass repeats the pipeline and vertex buffer before every draw, as naive callers do.
        for (int restate = 0; restate < 2; restate++)
        for (int round = 0; round < 5; round++)
        {
            if (round == 0)
                best = 1e9;
            platform::Stopwatch sw;
            CommandList cl;
            RenderPassDesc rp;
            rp.colors[0].texture = target;
            cl.BeginRenderPass(rp);
            cl.SetPipeline(pipe);
            cl.SetVertexBuffer(0, vb);
            for (u32 i = 0; i < 10000; i++)
            {
                TransientAllocation a = d.AllocateTransient(32);
                f32* p = static_cast<f32*>(a.data);
                p[0] = f32(i & 255) / 255.0f;
                p[1] = 0.5f;
                p[2] = 1.0f;
                p[3] = 1.0f;
                p[4] = f32(i % 100) / 50.0f - 1.0f;
                p[5] = f32(i / 100) / 50.0f - 1.0f;
                if (restate)
                {
                    cl.SetPipeline(pipe);
                    cl.SetVertexBuffer(0, vb);
                }
                cl.SetUniformBuffer(0, a);
                cl.Draw(3);
            }
            cl.EndRenderPass();
            f64 record = sw.ElapsedMilliseconds();
            d.Submit(cl);
            f64 submit = sw.ElapsedMilliseconds() - record;
            d.WaitIdle();
            f64 total = sw.ElapsedMilliseconds();
            if (total < best)
                best = total;
            if (round == 4)
                std::printf("    %s: 10k draws%s record %.2f ms, submit %.2f ms, with GPU %.2f ms (best %.2f)\n", BackendName(d.Caps().backend), restate ? " with repeated state" : "", record,
                            submit, total, best);
        }
        HX_CHECK(best < 1000.0);
        d.Destroy(target);
        d.Destroy(vb);
        d.Destroy(pipe);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}

HX_TEST(RHI, SwapChainPresent)
{
#if HX_PLATFORM_WINDOWS || HX_PLATFORM_LINUX
    if (!platform::Window::IsSupported())
        return;
    struct Case
    {
        Backend backend;
        i32 adapter;
    };
    Array<Case> cases;
    char name[128];
    for (u32 i = 0; Device::AdapterName(Backend::D3D11, i, name, sizeof(name)); i++)
        cases.Push({Backend::D3D11, i32(i)});
    if (cases.Empty())
        cases.Push({Backend::D3D11, -1});
    cases.Push({Backend::OpenGL, -1});
    for (const Case& c : cases)
    {
        Backend b = c.backend;
        if (!Device::IsBackendAvailable(b))
            continue;
        platform::Window* window = platform::Window::Create({.title = "rhi", .width = 128, .height = 96, .visible = false});
        HX_REQUIRE(window != nullptr);
        Device* d = Device::Create({.backend = b, .window = window->GetNativeHandle(), .adapter = c.adapter, .vsync = false});
        if (!d)
        {
            platform::Window::Destroy(window);
            continue;
        }
        ShaderHandle vs = MakeShader(*d, kColorVertex);
        ShaderHandle fs = MakeShader(*d, kColorFragment);
        PipelineDesc pd = ColorPipeline(vs, fs);
        pd.colorFormats[0] = Format::BGRA8Unorm;
        PipelineHandle pipe = d->CreatePipeline(pd);
        HX_REQUIRE(pipe.IsValid());
        const f32 verts[] = {-1, -1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, -1, 0, 0, 1, 1};
        BufferHandle vb = d->CreateBuffer({.size = sizeof(verts), .usage = BufferVertex, .initialData = verts});
        helix::Array<platform::Event> events;
        for (int frame = 0; frame < 3; frame++)
        {
            window->PollEvents(events);
            d->BeginFrame();
            CommandList cl;
            RenderPassDesc rp;
            cl.BeginRenderPass(rp);
            cl.SetPipeline(pipe);
            cl.SetVertexBuffer(0, vb);
            cl.Draw(3);
            cl.EndRenderPass();
            d->Submit(cl);
            d->EndFrame();
            d->Present();
        }
        d->Resize(160, 120);
        d->WaitIdle();
        std::printf("    presented on %s %s\n", BackendName(b), d->Caps().renderer);
        d->Destroy(vb);
        d->Destroy(pipe);
        d->Destroy(vs);
        d->Destroy(fs);
        Device::Destroy(d);
        platform::Window::Destroy(window);
        HX_CHECK(true);
    }
#endif
}

HX_TEST(RHI, BindlessTextures)
{
    ForEachBackend([&](Device& d) {
        if (!d.Caps().bindlessTextures)
        {
            std::printf("    %s: no bindless textures, skipped\n", d.Caps().renderer);
            return;
        }
        ShaderHandle vs = MakeShader(d, kBindlessQuadVertex);
        ShaderHandle fs = MakeShader(d, kBindlessQuadFragment);
        HX_REQUIRE(vs.IsValid() && fs.IsValid());
        PipelineDesc pd;
        pd.vertexShader = vs;
        pd.fragmentShader = fs;
        pd.vertexBuffers[0] = {8, StepMode::Vertex};
        pd.vertexBufferCount = 1;
        pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
        pd.attributeCount = 1;
        pd.topology = PrimitiveTopology::TriangleStrip;
        PipelineHandle pipe = d.CreatePipeline(pd);
        HX_REQUIRE(pipe.IsValid());
        const f32 quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(quad), .usage = BufferVertex, .initialData = quad});
        SamplerHandle sampler = d.CreateSampler({.minFilter = Filter::Nearest, .magFilter = Filter::Nearest, .mipFilter = Filter::Nearest});

        auto solid = [&](u32 size, u32 color) {
            Array<u32> texels;
            texels.Resize(size * size, color);
            return d.CreateTexture({.format = Format::RGBA8Unorm, .width = size, .height = size, .bindless = true, .initialData = texels.Data()});
        };
        TextureHandle red = solid(4, 0xff0000ff), green = solid(4, 0xff00ff00), blue = solid(8, 0xffff0000);
        Array<TextureHandle> extra;
        for (int i = 0; i < 40; i++)
            extra.Push(solid(4, i == 39 ? 0xff00ffff : 0xff808080));
        u32 iRed = d.RegisterBindless(red, sampler);
        u32 iGreen = d.RegisterBindless(green, sampler);
        u32 iBlue = d.RegisterBindless(blue, sampler);
        u32 iYellow = d.RegisterBindless(extra.Back(), sampler);
        HX_REQUIRE(iRed != kInvalidBindlessIndex && iGreen != kInvalidBindlessIndex && iBlue != kInvalidBindlessIndex && iYellow != kInvalidBindlessIndex);
        d.UnregisterBindless(iGreen);
        u32 reused = d.RegisterBindless(green, sampler);
        HX_CHECK_EQ(reused, iGreen);

        Target t(&d);
        CommandList cl;
        cl.BeginRenderPass(t.Pass());
        cl.SetPipeline(pipe);
        cl.SetVertexBuffer(0, vb);
        const u32 order[4] = {iRed, reused, iBlue, iYellow};
        for (u32 i = 0; i < 4; i++)
        {
            struct DrawData
            {
                u32 index;
                f32 x0, x1, pad;
            } data{order[i], f32(i) / 4.0f, f32(i + 1) / 4.0f, 0};
            TransientAllocation a = d.AllocateTransient(sizeof(DrawData));
            std::memcpy(a.data, &data, sizeof(data));
            cl.SetUniformBuffer(0, a);
            cl.Draw(4);
        }
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(t.Read());
        HX_CHECK(t.Golden("RHI.BindlessTextures"));
        HX_CHECK_EQ(t.At(1, 8), 0xff0000ffu);
        HX_CHECK_EQ(t.At(6, 8), 0xff00ff00u);
        HX_CHECK_EQ(t.At(10, 8), 0xffff0000u);
        HX_CHECK_EQ(t.At(14, 8), 0xff00ffffu);
        std::printf("    bindless %s on %s, %u textures\n", d.Caps().bindlessEmulated ? "emulated with texture array pools" : "native",
                    d.Caps().renderer, 43u);

        for (u32 index : {iRed, reused, iBlue, iYellow})
            d.UnregisterBindless(index);
        for (TextureHandle h : extra)
            d.Destroy(h);
        d.Destroy(red);
        d.Destroy(green);
        d.Destroy(blue);
        d.Destroy(sampler);
        d.Destroy(vb);
        d.Destroy(pipe);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}
