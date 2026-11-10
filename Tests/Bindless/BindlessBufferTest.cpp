#include <Support/CompiledShaders.h>
#include <Support/TestDevice.h>

#include <Helix/Testing/Test.h>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

namespace
{
    bool SkipWithoutBindlessBuffers(Device& d)
    {
        if (d.Caps().bindlessBuffers)
            return false;
        std::printf("    %s: no bindless buffers, skipped\n", d.Caps().renderer);
        return true;
    }

    struct DrawData
    {
        u32 index;
        f32 x0, x1, pad;
    };
}

// Colors and vertices both live in the heap: the vertex buffer binding and the shader table lookups must agree on offsets.
HX_TEST(BindlessBuffers, ShadersReadByIndex)
{
    ForEachBackend([&](Device& d) {
        if (SkipWithoutBindlessBuffers(d))
            return;
        ShaderHandle vs = MakeShader(d, kBindlessColorVertex);
        ShaderHandle fs = MakeShader(d, kBindlessColorFragment);
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

        BufferHandle gap = d.CreateBuffer({.size = 1000, .usage = BufferStorage, .bindless = true});
        const f32 quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(quad), .usage = BufferVertex, .bindless = true, .initialData = quad});
        auto colors = [&](f32 r, f32 g, f32 b, u32 extra) {
            Array<f32> data;
            data.Resize(8 + extra, 0.5f);
            const f32 c[4] = {r, g, b, 1};
            std::memcpy(data.Data() + 4, c, sizeof(c));
            return d.CreateBuffer({.size = data.Size() * 4, .usage = BufferStorage, .bindless = true, .initialData = data.Data()});
        };
        BufferHandle red = colors(1, 0, 0, 0), green = colors(0, 1, 0, 300), blue = colors(0, 0, 1, 70);
        d.Destroy(gap);
        BufferHandle yellow = colors(1, 1, 0, 0);
        HX_REQUIRE(vb.IsValid() && red.IsValid() && green.IsValid() && blue.IsValid() && yellow.IsValid());
        u32 iRed = d.RegisterBindlessBuffer(red), iGreen = d.RegisterBindlessBuffer(green);
        u32 iBlue = d.RegisterBindlessBuffer(blue), iYellow = d.RegisterBindlessBuffer(yellow);
        HX_REQUIRE(iRed != kInvalidBindlessIndex && iYellow != kInvalidBindlessIndex);
        HX_CHECK_EQ(d.RegisterBindlessBuffer(BufferHandle{}), kInvalidBindlessIndex);

        Target t(&d);
        auto render = [&] {
            CommandList cl;
            cl.BeginRenderPass(t.Pass());
            cl.SetPipeline(pipe);
            cl.SetVertexBuffer(0, vb);
            const u32 order[4] = {iRed, iGreen, iBlue, iYellow};
            for (u32 i = 0; i < 4; i++)
            {
                DrawData data{order[i], f32(i) / 4.0f, f32(i + 1) / 4.0f, 0};
                TransientAllocation a = d.AllocateTransient(sizeof(data));
                std::memcpy(a.data, &data, sizeof(data));
                cl.SetUniformBuffer(0, a);
                cl.Draw(4);
            }
            cl.EndRenderPass();
            d.Submit(cl);
            return t.Read();
        };
        HX_REQUIRE(render());
        HX_CHECK_EQ(t.At(1, 8), 0xff0000ffu);
        HX_CHECK_EQ(t.At(6, 8), 0xff00ff00u);
        HX_CHECK_EQ(t.At(10, 8), 0xffff0000u);
        HX_CHECK_EQ(t.At(14, 8), 0xff00ffffu);
        HX_CHECK(t.Golden("BindlessBuffers.ShadersReadByIndex"));

        const f32 white[4] = {1, 1, 1, 1};
        CommandList update;
        update.UpdateBuffer(blue, 16, white, sizeof(white));
        d.Submit(update);
        HX_REQUIRE(render());
        HX_CHECK_EQ(t.At(10, 8), 0xffffffffu);
        f32 back[4] = {};
        HX_REQUIRE(d.ReadBuffer(green, 16, back, sizeof(back)));
        HX_CHECK(back[0] == 0.0f && back[1] == 1.0f && back[3] == 1.0f);

        for (u32 i : {iRed, iGreen, iBlue, iYellow})
            d.UnregisterBindlessBuffer(i);
        for (BufferHandle h : {red, green, blue, yellow, vb})
            d.Destroy(h);
        d.Destroy(pipe);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}

HX_TEST(BindlessBuffers, ComputeWritesThroughHeap)
{
    ForEachBackend([&](Device& d) {
        if (SkipWithoutBindlessBuffers(d))
            return;
        ShaderHandle cs = MakeShader(d, kHeapWritesCompute);
        HX_REQUIRE(cs.IsValid());
        PipelineHandle pipe = d.CreateComputePipeline({cs, "heap writes"});
        HX_REQUIRE(pipe.IsValid());
        Array<u32> zeros;
        zeros.Resize(100, 0u);
        BufferHandle a = d.CreateBuffer({.size = 100 * 4, .usage = BufferStorage, .bindless = true, .initialData = zeros.Data()});
        BufferHandle b = d.CreateBuffer({.size = 50 * 4, .usage = BufferStorage, .bindless = true, .initialData = zeros.Data()});
        BufferHandle c = d.CreateBuffer({.size = 100 * 4, .usage = BufferStorage, .bindless = true, .initialData = zeros.Data()});
        u32 ia = d.RegisterBindlessBuffer(a), ib = d.RegisterBindlessBuffer(b);
        d.UnregisterBindlessBuffer(ia);
        u32 ic = d.RegisterBindlessBuffer(c);
        HX_CHECK_EQ(ic, ia);

        TransientAllocation p = d.AllocateTransient(16);
        const u32 params[4] = {ib, 0, 0, 0};
        std::memcpy(p.data, params, sizeof(params));
        CommandList cl;
        cl.SetPipeline(pipe);
        cl.SetUniformBuffer(0, p);
        cl.Dispatch(2);
        d.Submit(cl);

        u32 out[100] = {};
        HX_REQUIRE(d.ReadBuffer(b, 0, out, 50 * 4));
        HX_CHECK_EQ(out[0], ib);
        HX_CHECK_EQ(out[49], 49u * 3u + ib);
        HX_REQUIRE(d.ReadBuffer(a, 0, out, 100 * 4));
        HX_CHECK_EQ(out[0] + out[99], 0u);
        HX_REQUIRE(d.ReadBuffer(c, 0, out, 100 * 4));
        HX_CHECK_EQ(out[0] + out[99], 0u);

        d.UnregisterBindlessBuffer(ib);
        d.UnregisterBindlessBuffer(ic);
        d.Destroy(a);
        d.Destroy(b);
        d.Destroy(c);
        d.Destroy(pipe);
        d.Destroy(cs);
    });
}
