#include <Support/CompiledShaders.h>
#include <Support/TestDevice.h>

#include <Helix/Testing/Test.h>

#include <cstring>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

namespace
{
    // Two triangles over a rectangle of clip space.
    struct Quad
    {
        f32 v[12];
        Quad(f32 x0, f32 y0, f32 x1, f32 y1) : v{x0, y0, x0, y1, x1, y1, x0, y0, x1, y1, x1, y0} {}
    };

    // The Depth shaders' pipeline: float2 positions, a color and a depth from the uniform block.
    struct Drawer
    {
        Device& d;
        ShaderHandle vs, fs;
        BufferHandle full, left;

        explicit Drawer(Device& device, const CompiledShader& vertex = kDepthVertex, const CompiledShader& fragment = kDepthFragment) : d(device)
        {
            vs = MakeShader(d, vertex);
            fs = MakeShader(d, fragment);
            Quad a(-1, -1, 1, 1), b(-1, -1, 0, 1);
            full = d.CreateBuffer({.size = sizeof(a.v), .usage = BufferVertex, .initialData = a.v});
            left = d.CreateBuffer({.size = sizeof(b.v), .usage = BufferVertex, .initialData = b.v});
        }

        ~Drawer()
        {
            d.Destroy(full);
            d.Destroy(left);
            d.Destroy(vs);
            d.Destroy(fs);
        }

        PipelineDesc Desc(rhi::Format color = rhi::Format::RGBA8Unorm, rhi::Format depth = rhi::Format::Unknown) const
        {
            PipelineDesc pd;
            pd.vertexShader = vs;
            pd.fragmentShader = fs;
            pd.vertexBuffers[0] = {8, StepMode::Vertex};
            pd.vertexBufferCount = 1;
            pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
            pd.attributeCount = 1;
            pd.colorFormats[0] = color;
            pd.depthFormat = depth;
            return pd;
        }

        void Draw(CommandList& cl, PipelineHandle pipe, BufferHandle vb, const void* block, u32 size, u32 vertices = 6)
        {
            cl.SetPipeline(pipe);
            cl.SetVertexBuffer(0, vb);
            TransientAllocation a = d.AllocateTransient(size);
            std::memcpy(a.data, block, size);
            cl.SetUniformBuffer(0, a);
            cl.Draw(vertices);
        }

        void Draw(CommandList& cl, PipelineHandle pipe, BufferHandle vb, f32 r, f32 g, f32 b, f32 a, f32 depth)
        {
            const f32 block[8] = {r, g, b, a, depth, 0, 0, 0};
            Draw(cl, pipe, vb, block, sizeof(block));
        }
    };

    constexpr u32 kGreen = 0xFF00FF00u, kRed = 0xFF0000FFu, kBlack = 0xFF000000u;
}

HX_TEST(PipelineStates, StencilMasksTheSecondDraw)
{
    ForEachBackend([&](Device& d) {
        Drawer draw(d);
        TextureHandle color = d.CreateTexture({.format = Format::RGBA8Unorm, .width = kSize, .height = kSize, .usage = TextureRenderTarget | TextureSampled});
        TextureHandle depth = d.CreateTexture({.format = Format::D24UnormS8Uint, .width = kSize, .height = kSize, .usage = TextureDepthStencil});
        // The left half writes 1 into the stencil (no color); then green where the stencil equals 1.
        PipelineDesc mark = draw.Desc(Format::RGBA8Unorm, Format::D24UnormS8Uint);
        mark.blend[0].writeMask = 0;
        mark.stencil.enabled = true;
        mark.stencil.front.pass = mark.stencil.back.pass = StencilOp::Replace;
        PipelineDesc test = draw.Desc(Format::RGBA8Unorm, Format::D24UnormS8Uint);
        test.stencil.enabled = true;
        test.stencil.writeMask = 0;
        test.stencil.front.compare = test.stencil.back.compare = CompareFunction::Equal;
        PipelineHandle markPipe = d.CreatePipeline(mark), testPipe = d.CreatePipeline(test);
        HX_REQUIRE(markPipe.IsValid() && testPipe.IsValid());
        CommandList cl;
        RenderPassDesc rp;
        rp.colors[0].texture = color;
        rp.colors[0].clearColor[3] = 1;
        rp.depth.texture = depth;
        cl.BeginRenderPass(rp);
        cl.SetStencilReference(1);
        draw.Draw(cl, markPipe, draw.left, 1, 0, 0, 1, 0.5f);
        draw.Draw(cl, testPipe, draw.full, 0, 1, 0, 1, 0.5f);
        cl.EndRenderPass();
        d.Submit(cl);
        Array<u32> pixels;
        pixels.Resize(kSize * kSize);
        HX_REQUIRE(d.ReadTexture(color, 0, pixels.Data(), pixels.Size() * 4));
        HX_CHECK(pixels[8 * kSize + 3] == kGreen && pixels[8 * kSize + 12] == kBlack);
        d.Destroy(markPipe);
        d.Destroy(testPipe);
        d.Destroy(color);
        d.Destroy(depth);
    });
}

HX_TEST(PipelineStates, DepthBiasAndDepthClip)
{
    ForEachBackend([&](Device& d) {
        Drawer draw(d);
        Target target(&d, true);
        // A red quad, then a green one at the same depth with a less test: hidden unless biased toward the camera.
        PipelineDesc base = draw.Desc(Format::RGBA8Unorm, Format::D32Float);
        base.depthTest = base.depthWrite = true;
        base.depthCompare = CompareFunction::Less;
        PipelineDesc biased = base;
        biased.depthBias = -1000;
        biased.depthBiasSlope = -1;
        PipelineHandle plain = d.CreatePipeline(base), bias = d.CreatePipeline(biased);
        // Beyond the far plane: clipped, or clamped and drawn without depth clip.
        PipelineDesc far = draw.Desc(Format::RGBA8Unorm, Format::D32Float);
        PipelineDesc clamped = far;
        clamped.depthClip = false;
        PipelineHandle farPipe = d.CreatePipeline(far), clampPipe = d.CreatePipeline(clamped);
        HX_REQUIRE(plain.IsValid() && bias.IsValid() && farPipe.IsValid() && clampPipe.IsValid());
        u32 results[4];
        for (u32 run = 0; run < 4; run++)
        {
            CommandList cl;
            RenderPassDesc rp = target.Pass(0, 0, 0, 1);
            cl.BeginRenderPass(rp);
            if (run < 2)
            {
                draw.Draw(cl, plain, draw.full, 1, 0, 0, 1, 0.5f);
                draw.Draw(cl, run == 0 ? plain : bias, draw.full, 0, 1, 0, 1, 0.5f);
            }
            else
                draw.Draw(cl, run == 2 ? farPipe : clampPipe, draw.full, 0, 1, 0, 1, 1.5f);
            cl.EndRenderPass();
            d.Submit(cl);
            HX_REQUIRE(target.Read());
            results[run] = target.At(8, 8);
        }
        std::printf("    %s: depth clamp %d\n", BackendName(d.Caps().backend), d.Caps().depthClamp);
        HX_CHECK(results[0] == kRed && results[1] == kGreen && results[2] == kBlack);
        if (d.Caps().depthClamp)
            HX_CHECK(results[3] == kGreen);
        d.Destroy(plain);
        d.Destroy(bias);
        d.Destroy(farPipe);
        d.Destroy(clampPipe);
    });
}

HX_TEST(PipelineStates, ConservativeRasterizationCoversThinTriangles)
{
    ForEachBackend([&](Device& d) {
        std::printf("    %s: conservative %d\n", BackendName(d.Caps().backend), d.Caps().conservativeRasterization);
        if (!d.Caps().conservativeRasterization)
            return;
        Drawer draw(d);
        Target target(&d);
        // A sliver along a row's edge that misses every pixel center.
        const f32 sliver[6] = {-1, 0.01f, 1, 0.01f, 1, 0.02f};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(sliver), .usage = BufferVertex, .initialData = sliver});
        u32 lit[2] = {};
        for (u32 conservative = 0; conservative < 2; conservative++)
        {
            PipelineDesc pd = draw.Desc();
            pd.conservative = conservative != 0;
            PipelineHandle pipe = d.CreatePipeline(pd);
            HX_REQUIRE(pipe.IsValid());
            CommandList cl;
            cl.BeginRenderPass(target.Pass(0, 0, 0, 1));
            draw.Draw(cl, pipe, vb, 1, 1, 1, 1, 0.5f);
            cl.EndRenderPass();
            d.Submit(cl);
            HX_REQUIRE(target.Read());
            for (u32 i = 0; i < kSize * kSize; i++)
                lit[conservative] += target.pixels[i] != kBlack;
            d.Destroy(pipe);
        }
        HX_CHECK(lit[0] == 0 && lit[1] >= kSize);
        d.Destroy(vb);
    });
}

HX_TEST(PipelineStates, AlphaToCoverageCoversByAlpha)
{
    ForEachBackend([&](Device& d) {
        HX_REQUIRE(d.Caps().maxSamples >= 4);
        Drawer draw(d);
        u32 red[2];
        for (u32 a2c = 0; a2c < 2; a2c++)
        {
            PipelineDesc pd = draw.Desc();
            pd.sampleCount = 4;
            pd.alphaToCoverage = a2c != 0;
            PipelineHandle pipe = d.CreatePipeline(pd);
            TextureHandle msaa = d.CreateTexture({.format = Format::RGBA8Unorm, .width = kSize, .height = kSize, .sampleCount = 4, .usage = TextureRenderTarget});
            TextureHandle resolved = d.CreateTexture({.format = Format::RGBA8Unorm, .width = kSize, .height = kSize, .usage = TextureRenderTarget | TextureSampled});
            HX_REQUIRE(pipe.IsValid() && msaa.IsValid() && resolved.IsValid());
            CommandList cl;
            RenderPassDesc rp;
            rp.colors[0].texture = msaa;
            rp.colors[0].clearColor[3] = 1;
            cl.BeginRenderPass(rp);
            draw.Draw(cl, pipe, draw.full, 1, 1, 1, 0.5f, 0.5f);
            cl.EndRenderPass();
            cl.ResolveTexture(msaa, resolved);
            d.Submit(cl);
            Array<u32> pixels;
            pixels.Resize(kSize * kSize);
            HX_REQUIRE(d.ReadTexture(resolved, 0, pixels.Data(), pixels.Size() * 4));
            red[a2c] = pixels[8 * kSize + 8] & 0xFF;
            d.Destroy(pipe);
            d.Destroy(msaa);
            d.Destroy(resolved);
        }
        // Without it every sample is written; with alpha 0.5 about half of them.
        HX_CHECK(red[0] == 255 && red[1] > 64 && red[1] < 192);
    });
}

HX_TEST(PipelineStates, LogicalOperationsIntoIntegerTargets)
{
    ForEachBackend([&](Device& d) {
        std::printf("    %s: logic ops %d\n", BackendName(d.Caps().backend), d.Caps().logicOps);
        if (!d.Caps().logicOps)
            return;
        Drawer draw(d, kUintColorVertex, kUintColorFragment);
        TextureHandle target = d.CreateTexture({.format = Format::R32Uint, .width = kSize, .height = kSize, .usage = TextureRenderTarget | TextureSampled});
        PipelineDesc copy = draw.Desc(Format::R32Uint);
        PipelineDesc xorDesc = copy;
        xorDesc.logicOpEnabled = true;
        xorDesc.logicOp = LogicOp::Xor;
        PipelineHandle copyPipe = d.CreatePipeline(copy), xorPipe = d.CreatePipeline(xorDesc);
        HX_REQUIRE(target.IsValid() && copyPipe.IsValid() && xorPipe.IsValid());
        CommandList cl;
        RenderPassDesc rp;
        rp.colors[0].texture = target;
        rp.colors[0].load = LoadOp::Load;
        cl.BeginRenderPass(rp);
        const u32 first[4] = {0x0F0Fu, 0, 0, 0}, second[4] = {0x00FFu, 0, 0, 0};
        draw.Draw(cl, copyPipe, draw.full, first, sizeof(first));
        draw.Draw(cl, xorPipe, draw.left, second, sizeof(second));
        cl.EndRenderPass();
        d.Submit(cl);
        Array<u32> values;
        values.Resize(kSize * kSize);
        HX_REQUIRE(d.ReadTexture(target, 0, values.Data(), values.Size() * 4));
        HX_CHECK(values[8 * kSize + 3] == 0x0FF0u && values[8 * kSize + 12] == 0x0F0Fu);
        d.Destroy(copyPipe);
        d.Destroy(xorPipe);
        d.Destroy(target);
    });
}

HX_TEST(PipelineStates, UniformBuffersUpdatedInPartsAndBoundByOffset)
{
    ForEachBackend([&](Device& d) {
        std::printf("    %s: offsets %d, partial updates %d\n", BackendName(d.Caps().backend), d.Caps().uniformBufferOffsets,
                    d.Caps().uniformBufferPartialUpdates);
        if (!d.Caps().uniformBufferOffsets || !d.Caps().uniformBufferPartialUpdates)
            return;
        Drawer draw(d);
        Target target(&d);
        PipelineHandle pipe = d.CreatePipeline(draw.Desc());
        // Two blocks in one persistent buffer, written separately; the second one bound by its offset.
        BufferHandle blocks = d.CreateBuffer({.size = 512, .usage = BufferUniform});
        HX_REQUIRE(pipe.IsValid() && blocks.IsValid());
        const f32 red[8] = {1, 0, 0, 1, 0.5f, 0, 0, 0}, green[8] = {0, 1, 0, 1, 0.5f, 0, 0, 0};
        CommandList cl;
        cl.UpdateBuffer(blocks, 0, red, sizeof(red));
        cl.UpdateBuffer(blocks, 256, green, sizeof(green));
        cl.BeginRenderPass(target.Pass(0, 0, 0, 1));
        cl.SetPipeline(pipe);
        cl.SetVertexBuffer(0, draw.left);
        cl.SetUniformBuffer(0, blocks, 0, 256);
        cl.Draw(6);
        Quad right(0, -1, 1, 1);
        BufferHandle rightVb = d.CreateBuffer({.size = sizeof(right.v), .usage = BufferVertex, .initialData = right.v});
        cl.SetVertexBuffer(0, rightVb);
        cl.SetUniformBuffer(0, blocks, 256, 256);
        cl.Draw(6);
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(target.Read());
        if (!(target.At(3, 8) == kRed && target.At(12, 8) == kGreen))
            std::printf("    %08x %08x\n", target.At(3, 8), target.At(12, 8));
        HX_CHECK(target.At(3, 8) == kRed && target.At(12, 8) == kGreen);
        d.Destroy(rightVb);
        d.Destroy(blocks);
        d.Destroy(pipe);
    });
}

HX_TEST(PipelineStates, InstancesReadTheirIndex)
{
    ForEachBackend([&](Device& d) {
        // Four instances of a quarter wide quad, each half a clip unit further right with its own color.
        Drawer draw(d, kInstanceRowVertex, kInstanceRowFragment);
        Target target(&d);
        Quad first(-1, -1, -0.5f, 1);
        BufferHandle vb = d.CreateBuffer({.size = sizeof(first.v), .usage = BufferVertex, .initialData = first.v});
        PipelineHandle pipe = d.CreatePipeline(draw.Desc());
        HX_REQUIRE(vb.IsValid() && pipe.IsValid());
        const f32 colors[16] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1};
        CommandList cl;
        cl.BeginRenderPass(target.Pass(0, 0, 0, 1));
        cl.SetPipeline(pipe);
        cl.SetVertexBuffer(0, vb);
        TransientAllocation a = d.AllocateTransient(sizeof(colors));
        std::memcpy(a.data, colors, sizeof(colors));
        cl.SetUniformBuffer(0, a);
        cl.Draw(6, 4);
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(target.Read());
        HX_CHECK(target.At(2, 8) == kRed && target.At(6, 8) == kGreen && target.At(10, 8) == 0xFFFF0000u && target.At(14, 8) == 0xFFFFFFFFu);
        d.Destroy(vb);
        d.Destroy(pipe);
    });
}
