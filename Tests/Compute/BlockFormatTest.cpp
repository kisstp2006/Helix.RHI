#include <Support/CompiledShaders.h>
#include <Support/TestDevice.h>

#include <Helix/Image/Texture/BlockCodec.h>
#include <Helix/Image/Texture/Convert.h>
#include <Helix/Testing/Test.h>

#include <cmath>
#include <cstring>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

namespace
{
    enum class Family
    {
        BC,
        RGTC,
        BPTC,
        ETC2,
        ASTC,
    };

    struct Case
    {
        rhi::Format format;
        image::PixelFormat image;
        Family family;
        // Channels the format has; the others are not compared.
        u32 channels;
        // The largest difference allowed, in the values the GPU returns.
        float tolerance;
        // Random blocks are valid blocks of this format (ASTC ones mostly are not and decode to the error color).
        bool randomBlocks;
    };

    constexpr float kByte = 1.0f / 255.0f, kEleven = 1.0f / 2047.0f;
    // BC1 to BC5 leave the precision of the interpolation to the hardware: AMD interpolates the exact weights (BC4 and BC5
    // in float), NVIDIA rounds the weights to sixteenths (2/3 is 11/16, 2/5 is 6/16), which moves a value by up to 1/32 of
    // the endpoint range, about 8 steps. A wrong index or mode would be a whole palette step off.
    constexpr float kBcByte = 8.5f / 255.0f, kBcSigned = 8.5f / 127.0f;

    const Case kCases[] = {
        {rhi::Format::BC1Unorm, image::PixelFormat::BC1, Family::BC, 4, kBcByte, true},
        {rhi::Format::BC2Unorm, image::PixelFormat::BC2, Family::BC, 4, kBcByte, true},
        {rhi::Format::BC3Unorm, image::PixelFormat::BC3, Family::BC, 4, kBcByte, true},
        {rhi::Format::BC4Unorm, image::PixelFormat::BC4, Family::RGTC, 1, kBcByte, true},
        {rhi::Format::BC4Snorm, image::PixelFormat::BC4Snorm, Family::RGTC, 1, kBcSigned, true},
        {rhi::Format::BC5Unorm, image::PixelFormat::BC5, Family::RGTC, 2, kBcByte, true},
        {rhi::Format::BC5Snorm, image::PixelFormat::BC5Snorm, Family::RGTC, 2, kBcSigned, true},
        {rhi::Format::BC6HUfloat, image::PixelFormat::BC6H, Family::BPTC, 3, 0.0f, true},
        {rhi::Format::BC6HSfloat, image::PixelFormat::BC6HSigned, Family::BPTC, 3, 0.0f, true},
        {rhi::Format::BC7Unorm, image::PixelFormat::BC7, Family::BPTC, 4, kByte, true},
        {rhi::Format::ETC2RGB8, image::PixelFormat::ETC2RGB, Family::ETC2, 3, kByte, true},
        {rhi::Format::ETC2RGB8A1, image::PixelFormat::ETC2RGBA1, Family::ETC2, 4, kByte, true},
        {rhi::Format::ETC2RGBA8, image::PixelFormat::ETC2RGBA, Family::ETC2, 4, kByte, true},
        {rhi::Format::EACR11, image::PixelFormat::EACR11, Family::ETC2, 1, kEleven, true},
        {rhi::Format::EACR11Snorm, image::PixelFormat::EACR11Snorm, Family::ETC2, 1, kEleven, true},
        {rhi::Format::EACRG11, image::PixelFormat::EACRG11, Family::ETC2, 2, kEleven, true},
        {rhi::Format::EACRG11Snorm, image::PixelFormat::EACRG11Snorm, Family::ETC2, 2, kEleven, true},
        {rhi::Format::ASTC4x4, image::PixelFormat::ASTC4x4, Family::ASTC, 4, kByte, false},
        {rhi::Format::ASTC5x5, image::PixelFormat::ASTC5x5, Family::ASTC, 4, kByte, false},
        {rhi::Format::ASTC6x6, image::PixelFormat::ASTC6x6, Family::ASTC, 4, kByte, false},
        {rhi::Format::ASTC8x5, image::PixelFormat::ASTC8x5, Family::ASTC, 4, kByte, false},
        {rhi::Format::ASTC10x10, image::PixelFormat::ASTC10x10, Family::ASTC, 4, kByte, false},
        {rhi::Format::ASTC12x12, image::PixelFormat::ASTC12x12, Family::ASTC, 4, kByte, false},
    };

    bool Supported(const DeviceCaps& caps, Family f)
    {
        switch (f)
        {
        case Family::BC: return caps.textureCompressionBC;
        case Family::RGTC: return caps.textureCompressionRGTC;
        case Family::BPTC: return caps.textureCompressionBPTC;
        case Family::ETC2: return caps.textureCompressionETC2;
        default: return caps.textureCompressionASTC;
        }
    }

    // 120 x 120 texels (whole blocks of every size): the upper half a pattern of gradients, edges and alpha (values up to
    // 8 and below 0 for the HDR formats), the lower half random blocks where they are valid, else more of the pattern.
    image::TextureData Blocks(const Case& c)
    {
        constexpr u32 kSide = 120;
        image::TextureData source;
        source.Allocate(image::PixelFormat::RGBA32Float, kSide, kSide);
        float* f = reinterpret_cast<float*>(source.data.Data());
        bool hdr = c.image == image::PixelFormat::BC6H || c.image == image::PixelFormat::BC6HSigned;
        bool negative = c.image == image::PixelFormat::BC6HSigned || c.image == image::PixelFormat::BC4Snorm || c.image == image::PixelFormat::BC5Snorm ||
                        c.image == image::PixelFormat::EACR11Snorm || c.image == image::PixelFormat::EACRG11Snorm;
        for (u32 y = 0; y < kSide; y++)
            for (u32 x = 0; x < kSide; x++)
            {
                float* p = f + (usize(y) * kSide + x) * 4;
                bool edge = ((x / 7) + (y / 5)) & 1;
                p[0] = 0.5f + 0.45f * std::sin(float(x) * 0.11f + float(y) * 0.03f);
                p[1] = edge ? 0.9f : 0.15f;
                p[2] = float(x ^ y) / 128.0f;
                p[3] = x < 60 ? 1.0f : float(y % 40) / 39.0f;
                for (int k = 0; k < 3; k++)
                {
                    if (hdr)
                        p[k] = p[k] * 8.0f;
                    if (negative)
                        p[k] = p[k] * 2.0f - 1.0f;
                }
            }
        image::TextureData blocks;
        String error;
        image::CompressTexture(source, c.image, image::CompressionQuality::Normal, blocks, error);
        if (c.randomBlocks)
        {
            u64 state = 0x9E3779B97F4A7C15ull ^ u64(c.image);
            usize half = blocks.data.Size() / 2;
            for (usize i = half; i < blocks.data.Size(); i++)
            {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                blocks.data[i] = u8(state >> 24);
            }
        }
        return blocks;
    }
}

HX_TEST(Compute, BlockFormatsMatchTheImageDecoders)
{
    ForEachBackend([&](Device& d) {
        if (!d.Caps().compute || !d.Caps().storageBuffers)
        {
            std::printf("    %s: no compute shaders, skipped\n", d.Caps().renderer);
            return;
        }
        ShaderHandle cs = MakeShader(d, kLoadTexelsCompute);
        HX_REQUIRE(cs.IsValid());
        PipelineHandle pipe = d.CreateComputePipeline({cs, "load texels"});
        SamplerHandle sampler = d.CreateSampler({});
        u32 tested = 0;
        for (const Case& c : kCases)
        {
            const char* name = image::FormatInfo(c.image).name.Data();
            if (!Supported(d.Caps(), c.family))
            {
                std::printf("    %s: %.*s not offered, skipped\n", d.Caps().renderer, int(image::FormatInfo(c.image).name.Size()), name);
                continue;
            }
            image::TextureData blocks = Blocks(c);
            image::TextureData expected;
            String error;
            HX_REQUIRE(image::DecompressTexture(blocks, expected, error));
            u32 w = blocks.width, h = blocks.height;
            Array<float> ours;
            ours.Resize(usize(w) * h * 4);
            image::ToFloat(expected.format, expected.data.Data(), ours.Data(), usize(w) * h);

            TextureHandle texture = d.CreateTexture({.format = c.format, .width = w, .height = h, .initialData = blocks.data.Data()});
            HX_REQUIRE(texture.IsValid());
            BufferHandle out = d.CreateBuffer({.size = u64(w) * h * 16, .usage = BufferStorage});
            TransientAllocation a = d.AllocateTransient(16);
            const u32 size[4] = {w, h, 0, 0};
            std::memcpy(a.data, size, sizeof(size));
            CommandList cl;
            cl.SetPipeline(pipe);
            cl.SetUniformBuffer(0, a);
            cl.SetTexture(0, texture, sampler);
            cl.SetStorageBuffer(0, out);
            cl.Dispatch((w + 7) / 8, (h + 7) / 8);
            d.Submit(cl);
            Array<float> gpu;
            gpu.Resize(usize(w) * h * 4);
            HX_REQUIRE(d.ReadBuffer(out, 0, gpu.Data(), u32(gpu.Size() * 4)));

            float worst = 0.0f;
            usize at = 0;
            for (usize i = 0; i < usize(w) * h; i++)
                for (u32 ch = 0; ch < c.channels; ch++)
                {
                    float a0 = ours[i * 4 + ch], b0 = gpu[i * 4 + ch];
                    // BC6H: within one step of the half value.
                    float allowed = c.tolerance > 0.0f ? c.tolerance : std::fabs(a0) * 0.001f + 1e-6f;
                    float diff = std::fabs(a0 - b0);
                    if (diff - allowed > worst - (c.tolerance > 0.0f ? c.tolerance : 0.0f) && diff > allowed)
                        at = i;
                    worst = diff > worst ? diff : worst;
                }
            bool ok = true;
            for (usize i = 0; i < usize(w) * h && ok; i++)
                for (u32 ch = 0; ch < c.channels; ch++)
                {
                    float a0 = ours[i * 4 + ch], b0 = gpu[i * 4 + ch];
                    float allowed = (c.tolerance > 0.0f ? c.tolerance : std::fabs(a0) * 0.001f + 1e-6f) * 1.01f;
                    if (std::fabs(a0 - b0) > allowed)
                    {
                        ok = false;
                        at = i;
                    }
                }
            std::printf("    %s: %-12.*s largest difference %.5f%s\n", d.Caps().renderer, int(image::FormatInfo(c.image).name.Size()), name, worst,
                        ok ? "" : " (too large)");
            if (!ok)
                std::printf("      texel %zu: ours %.5f %.5f %.5f %.5f, GPU %.5f %.5f %.5f %.5f\n", at, ours[at * 4], ours[at * 4 + 1], ours[at * 4 + 2],
                            ours[at * 4 + 3], gpu[at * 4], gpu[at * 4 + 1], gpu[at * 4 + 2], gpu[at * 4 + 3]);
            HX_CHECK(ok);
            tested++;
            d.Destroy(out);
            d.Destroy(texture);
        }
        std::printf("    %s: %u block formats compared\n", d.Caps().renderer, tested);
        d.Destroy(sampler);
        d.Destroy(pipe);
        d.Destroy(cs);
    });
}
