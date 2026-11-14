#include <Support/CompiledShaders.h>
#include <Support/TestDevice.h>

#include <Helix/Testing/Test.h>

#include <cstring>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

namespace
{
    constexpr u32 Rgba(u32 r, u32 g, u32 b, u32 a = 255) { return r | (g << 8) | (b << 16) | (a << 24); }

    bool Near(u32 a, u32 b, int tolerance)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            int ca = int((a >> shift) & 0xFF), cb = int((b >> shift) & 0xFF);
            if (ca - cb > tolerance || cb - ca > tolerance)
                return false;
        }
        return true;
    }

    // One clear pass into a mip and layer of a texture.
    void ClearInto(CommandList& cl, TextureHandle texture, u32 mip, u32 layer, f32 r, f32 g, f32 b)
    {
        RenderPassDesc rp;
        rp.colors[0].texture = texture;
        rp.colors[0].mipLevel = mip;
        rp.colors[0].layer = layer;
        rp.colors[0].clearColor[0] = r, rp.colors[0].clearColor[1] = g, rp.colors[0].clearColor[2] = b, rp.colors[0].clearColor[3] = 1;
        cl.BeginRenderPass(rp);
        cl.EndRenderPass();
    }

    u32 ReadOne(Device& d, TextureHandle t, u32 mip, u32 x, u32 y, u32 width)
    {
        Array<u32> pixels;
        pixels.Resize(usize(width) * width);
        if (!d.ReadTexture(t, mip, pixels.Data(), pixels.Size() * 4))
            return 0xDEADBEEFu;
        return pixels[usize(y) * width + x];
    }
}

HX_TEST(RenderTargets, MultisampledTargetsResolve)
{
    ForEachBackend([&](Device& d) {
        std::printf("    %s: %u samples\n", BackendName(d.Caps().backend), d.Caps().maxSamples);
        HX_REQUIRE(d.Caps().maxSamples >= 4);
        ShaderHandle vs = MakeShader(d, kDepthVertex);
        ShaderHandle fs = MakeShader(d, kDepthFragment);
        // A triangle whose long edge crosses the target diagonally, white on black.
        const f32 verts[] = {-1, -1, 1, 1, 1, -1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(verts), .usage = BufferVertex, .initialData = verts});
        u32 results[2][kSize * kSize];
        for (u32 samples : {1u, 4u})
        {
            PipelineDesc pd;
            pd.vertexShader = vs;
            pd.fragmentShader = fs;
            pd.vertexBuffers[0] = {8, StepMode::Vertex};
            pd.vertexBufferCount = 1;
            pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
            pd.attributeCount = 1;
            pd.colorFormats[0] = Format::RGBA8Unorm;
            pd.sampleCount = samples;
            PipelineHandle pipe = d.CreatePipeline(pd);
            HX_REQUIRE(pipe.IsValid());
            TextureHandle msaa =
                d.CreateTexture({.format = Format::RGBA8Unorm, .width = kSize, .height = kSize, .sampleCount = samples, .usage = TextureRenderTarget});
            TextureHandle resolved =
                d.CreateTexture({.format = Format::RGBA8Unorm, .width = kSize, .height = kSize, .usage = TextureRenderTarget | TextureSampled});
            HX_REQUIRE(msaa.IsValid() && resolved.IsValid());
            CommandList cl;
            RenderPassDesc rp;
            rp.colors[0].texture = msaa;
            rp.colors[0].clearColor[3] = 1;
            cl.BeginRenderPass(rp);
            cl.SetPipeline(pipe);
            cl.SetVertexBuffer(0, vb);
            const f32 draw[8] = {1, 1, 1, 1, 0.5f, 0, 0, 0};
            TransientAllocation a = d.AllocateTransient(sizeof(draw));
            std::memcpy(a.data, draw, sizeof(draw));
            cl.SetUniformBuffer(0, a);
            cl.Draw(3);
            cl.EndRenderPass();
            if (samples > 1)
                cl.ResolveTexture(msaa, resolved);
            else
                cl.CopyTexture(msaa, 0, 0, resolved, 0, 0);
            d.Submit(cl);
            HX_REQUIRE(d.ReadTexture(resolved, 0, results[samples > 1], sizeof(results[0])));
            d.Destroy(msaa);
            d.Destroy(resolved);
            d.Destroy(pipe);
        }
        // Without samples every pixel is in or out; with four the edge's pixels are partly covered.
        u32 partialSingle = 0, partialMulti = 0, white = 0;
        for (u32 i = 0; i < kSize * kSize; i++)
        {
            u32 r1 = results[0][i] & 0xFF, r4 = results[1][i] & 0xFF;
            partialSingle += r1 != 0 && r1 != 255;
            partialMulti += r4 > 20 && r4 < 235;
            white += r4 >= 235;
        }
        HX_CHECK(partialSingle == 0 && partialMulti >= kSize / 2 && white > kSize * kSize / 4);
        d.Destroy(vb);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}

HX_TEST(RenderTargets, MipsLayersCopiesAndGeneratedMips)
{
    ForEachBackend([&](Device& d) {
        CommandList cl;
        // A 4 x 4 texture of four colored quadrants with three mips: the second mip has the quadrants' colors, the last
        // their average.
        u32 texels[16];
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++)
                texels[y * 4 + x] = y < 2 ? (x < 2 ? Rgba(255, 0, 0) : Rgba(0, 255, 0)) : (x < 2 ? Rgba(0, 0, 255) : Rgba(255, 255, 255));
        TextureHandle mipped = d.CreateTexture({.format = Format::RGBA8Unorm, .width = 4, .height = 4, .mipLevels = 3,
                                                .usage = TextureSampled | TextureRenderTarget, .initialData = texels});
        HX_REQUIRE(mipped.IsValid());
        cl.GenerateMips(mipped);
        // The second mip copied into a texture of its own.
        TextureHandle copy = d.CreateTexture({.format = Format::RGBA8Unorm, .width = 2, .height = 2, .usage = TextureSampled | TextureRenderTarget});
        cl.CopyTexture(mipped, 1, 0, copy, 0, 0);
        // A target's second mip, and the layers of an array, each cleared on its own.
        TextureHandle mipTarget = d.CreateTexture({.format = Format::RGBA8Unorm, .width = 4, .height = 4, .mipLevels = 2,
                                                   .usage = TextureSampled | TextureRenderTarget});
        ClearInto(cl, mipTarget, 0, 0, 1, 0, 0);
        ClearInto(cl, mipTarget, 1, 0, 0, 0, 1);
        TextureHandle array = d.CreateTexture({.type = TextureType::Texture2DArray, .format = Format::RGBA8Unorm, .width = 4, .height = 4,
                                               .depthOrLayers = 2, .usage = TextureSampled | TextureRenderTarget});
        ClearInto(cl, array, 0, 0, 1, 0, 0);
        ClearInto(cl, array, 0, 1, 0, 1, 0);
        TextureHandle layer1 = d.CreateTexture({.format = Format::RGBA8Unorm, .width = 4, .height = 4, .usage = TextureSampled | TextureRenderTarget});
        cl.CopyTexture(array, 0, 1, layer1, 0, 0);
        d.Submit(cl);

        u32 quadrant[4];
        Array<u32> two;
        two.Resize(4);
        HX_REQUIRE(d.ReadTexture(mipped, 1, two.Data(), 16));
        std::memcpy(quadrant, two.Data(), 16);
        HX_CHECK(Near(quadrant[0], Rgba(255, 0, 0), 2) && Near(quadrant[1], Rgba(0, 255, 0), 2) && Near(quadrant[2], Rgba(0, 0, 255), 2) &&
                 Near(quadrant[3], Rgba(255, 255, 255), 2));
        u32 last = 0;
        HX_REQUIRE(d.ReadTexture(mipped, 2, &last, 4));
        HX_CHECK(Near(last, Rgba(128, 128, 128), 3));
        HX_REQUIRE(d.ReadTexture(copy, 0, two.Data(), 16));
        HX_CHECK(std::memcmp(two.Data(), quadrant, 16) == 0);
        HX_CHECK(ReadOne(d, mipTarget, 0, 1, 1, 4) == Rgba(255, 0, 0) && ReadOne(d, mipTarget, 1, 1, 1, 2) == Rgba(0, 0, 255));
        HX_CHECK(ReadOne(d, layer1, 0, 2, 2, 4) == Rgba(0, 255, 0));
        for (TextureHandle t : {mipped, copy, mipTarget, array, layer1})
            d.Destroy(t);
    });
}
