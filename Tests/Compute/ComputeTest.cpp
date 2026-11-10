#include <Support/CompiledShaders.h>
#include <Support/TestDevice.h>

#include <Helix/Testing/Test.h>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

namespace
{
    bool SkipWithoutCompute(Device& d)
    {
        if (d.Caps().compute && d.Caps().storageBuffers)
            return false;
        std::printf("    %s: no compute shaders, skipped\n", d.Caps().renderer);
        return true;
    }
}

HX_TEST(Compute, WritesStorageBuffer)
{
    ForEachBackend([&](Device& d) {
        if (SkipWithoutCompute(d))
            return;
        ShaderHandle cs = MakeShader(d, kFillCompute);
        HX_REQUIRE(cs.IsValid());
        PipelineHandle pipe = d.CreateComputePipeline({cs, "fill"});
        HX_REQUIRE(pipe.IsValid());
        BufferHandle out = d.CreateBuffer({.size = 256 * 4, .usage = BufferStorage});
        const DispatchIndirectArgs half{2, 1, 1};
        BufferHandle args = d.CreateBuffer({.size = sizeof(half), .usage = BufferIndirect | BufferStorage, .initialData = &half});

        auto run = [&](u32 k, bool indirect) {
            TransientAllocation a = d.AllocateTransient(16);
            const u32 data[4] = {k, 0, 0, 0};
            std::memcpy(a.data, data, sizeof(data));
            CommandList cl;
            cl.SetPipeline(pipe);
            cl.SetUniformBuffer(0, a);
            cl.SetStorageBuffer(0, out);
            if (indirect)
                cl.DispatchIndirect(args);
            else
                cl.Dispatch(4);
            d.Submit(cl);
        };
        run(7, false);
        run(1000, true);
        u32 values[256] = {};
        HX_REQUIRE(d.ReadBuffer(out, 0, values, sizeof(values)));
        HX_CHECK_EQ(values[0], 1000u);
        HX_CHECK_EQ(values[127], 127u * 127u + 1000u);
        HX_CHECK_EQ(values[128], 128u * 128u + 7u);
        HX_CHECK_EQ(values[255], 255u * 255u + 7u);

        d.Destroy(args);
        d.Destroy(out);
        d.Destroy(pipe);
        d.Destroy(cs);
    });
}

HX_TEST(Compute, WritesStorageTexture)
{
    ForEachBackend([&](Device& d) {
        if (SkipWithoutCompute(d))
            return;
        ShaderHandle cs = MakeShader(d, kGradientCompute);
        HX_REQUIRE(cs.IsValid());
        PipelineHandle pipe = d.CreateComputePipeline({cs, "gradient"});
        HX_REQUIRE(pipe.IsValid());
        TextureHandle tex = d.CreateTexture({.format = Format::RGBA8Unorm, .width = kSize, .height = kSize, .usage = TextureStorage | TextureSampled});
        CommandList cl;
        cl.SetPipeline(pipe);
        cl.SetStorageTexture(0, tex);
        cl.Dispatch(kSize / 8, kSize / 8);
        d.Submit(cl);
        u32 pixels[kSize * kSize] = {};
        HX_REQUIRE(d.ReadTexture(tex, 0, pixels, sizeof(pixels)));
        HX_CHECK_EQ(pixels[0], 0xff000000u);
        HX_CHECK_EQ(pixels[15], 0xff0000ffu);
        HX_CHECK_EQ(pixels[15 * kSize], 0xff00ff00u);
        HX_CHECK_EQ(pixels[15 * kSize + 15], 0xff00ffffu);
        HX_CHECK(MatchGolden("Compute.WritesStorageTexture", pixels, kSize, kSize));

        d.Destroy(tex);
        d.Destroy(pipe);
        d.Destroy(cs);
    });
}

// A compute pass culls quads, appends their vertices and the draw count; the draw is issued from the GPU written arguments.
HX_TEST(Compute, GpuDrivenIndirectDraw)
{
    ForEachBackend([&](Device& d) {
        if (SkipWithoutCompute(d) || !d.Caps().drawIndirect)
            return;
        ShaderHandle cs = MakeShader(d, kCullCompute);
        ShaderHandle vs = MakeShader(d, kPlainVertex);
        ShaderHandle fs = MakeShader(d, kGreenFragment);
        HX_REQUIRE(cs.IsValid() && vs.IsValid() && fs.IsValid());
        PipelineHandle cull = d.CreateComputePipeline({cs, "cull"});
        PipelineDesc pd;
        pd.vertexShader = vs;
        pd.fragmentShader = fs;
        pd.vertexBuffers[0] = {8, StepMode::Vertex};
        pd.vertexBufferCount = 1;
        pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
        pd.attributeCount = 1;
        PipelineHandle pipe = d.CreatePipeline(pd);
        HX_REQUIRE(cull.IsValid() && pipe.IsValid());

        const u32 visible[4] = {1, 0, 1, 1};
        const DrawIndirectArgs initial{0, 1, 0, 0};
        BufferHandle verts = d.CreateBuffer({.size = 4 * 6 * 8, .usage = BufferVertex | BufferStorage});
        BufferHandle flags = d.CreateBuffer({.size = sizeof(visible), .usage = BufferStorage, .initialData = visible});
        BufferHandle args = d.CreateBuffer({.size = sizeof(initial), .usage = BufferIndirect | BufferStorage, .initialData = &initial});

        Target t(&d);
        CommandList cl;
        cl.SetPipeline(cull);
        cl.SetStorageBuffer(0, verts);
        cl.SetStorageBuffer(1, flags, StorageAccess::ReadOnly);
        cl.SetStorageBuffer(2, args);
        cl.Dispatch(1);
        cl.BeginRenderPass(t.Pass());
        cl.SetPipeline(pipe);
        cl.SetVertexBuffer(0, verts);
        cl.DrawIndirect(args);
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(t.Read());
        HX_CHECK(t.Golden("Compute.GpuDrivenIndirectDraw"));
        HX_CHECK_EQ(t.At(2, 8), 0xff00ff00u);
        HX_CHECK_EQ(t.At(6, 8), 0u);
        HX_CHECK_EQ(t.At(10, 8), 0xff00ff00u);
        HX_CHECK_EQ(t.At(14, 8), 0xff00ff00u);
        DrawIndirectArgs written{};
        HX_REQUIRE(d.ReadBuffer(args, 0, &written, sizeof(written)));
        HX_CHECK_EQ(written.vertexCount, 18u);

        d.Destroy(args);
        d.Destroy(flags);
        d.Destroy(verts);
        d.Destroy(pipe);
        d.Destroy(cull);
        d.Destroy(vs);
        d.Destroy(fs);
        d.Destroy(cs);
    });
}
