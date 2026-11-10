#pragma once

#include <Support/CompiledShader.h>
#include <Support/Golden.h>

#include <Helix/Platform/Window/Window.h>

#include <cstdio>

namespace helix::rhi::testing
{
    inline constexpr u32 kSize = 16;

    inline const char* BackendName(Backend b)
    {
        switch (b)
        {
        case Backend::D3D11: return "D3D11";
        case Backend::OpenGL: return "OpenGL";
        case Backend::OpenGLES: return "OpenGL ES";
        case Backend::WebGL2: return "WebGL 2";
        }
        return "?";
    }

    struct Target
    {
        Device* device;
        u32 size;
        TextureHandle color;
        TextureHandle depth;
        Array<u32> pixels;

        explicit Target(Device* d, bool withDepth = false, u32 targetSize = kSize) : device(d), size(targetSize)
        {
            pixels.Resize(usize(size) * size);
            color = d->CreateTexture({.format = Format::RGBA8Unorm, .width = size, .height = size, .usage = TextureRenderTarget | TextureSampled});
            if (withDepth)
                depth = d->CreateTexture({.format = Format::D32Float, .width = size, .height = size, .usage = TextureDepthStencil});
        }

        ~Target()
        {
            device->Destroy(color);
            device->Destroy(depth);
        }

        RenderPassDesc Pass(f32 r = 0, f32 g = 0, f32 b = 0, f32 a = 0) const
        {
            RenderPassDesc rp;
            rp.colors[0].texture = color;
            rp.colors[0].clearColor[0] = r;
            rp.colors[0].clearColor[1] = g;
            rp.colors[0].clearColor[2] = b;
            rp.colors[0].clearColor[3] = a;
            rp.depth.texture = depth;
            return rp;
        }

        bool Read() { return device->ReadTexture(color, 0, pixels.Data(), pixels.Size() * 4); }
        u32 At(u32 x, u32 y) const { return pixels[y * size + x]; }
        bool Golden(const char* name) const { return MatchGolden(name, pixels.Data(), size, size); }
    };

    // Float to UNORM8 conversion may round either way within the tolerance the APIs allow.
    inline bool NearColor(u32 a, u32 b)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            int ca = int((a >> shift) & 0xFF), cb = int((b >> shift) & 0xFF);
            if (ca - cb > 1 || cb - ca > 1)
                return false;
        }
        return true;
    }

    template <typename F>
    void ForEachBackend(F&& f, u32 transientBufferSize = 8u << 20)
    {
        Backend list[] = {Backend::D3D11, Backend::OpenGL, Backend::WebGL2};
        for (Backend b : list)
        {
            if (b == Backend::WebGL2 && !platform::Window::IsSupported())
                continue;
            if (!Device::IsBackendAvailable(b))
                continue;
            char name[128];
            u32 adapters = 0;
            while (b == Backend::D3D11 && Device::AdapterName(b, adapters, name, sizeof(name)))
                adapters++;
            for (i32 adapter = adapters ? 0 : -1; adapter < i32(adapters); adapter++)
            {
                Device* device = Device::Create({.backend = b, .adapter = adapter, .vsync = false, .transientBufferSize = transientBufferSize});
                if (!device)
                {
                    std::printf("    %s: device creation failed, skipped\n", BackendName(b));
                    continue;
                }
                f(*device);
                Device::Destroy(device);
            }
        }
    }

    inline PipelineDesc ColorPipeline(ShaderHandle vs, ShaderHandle fs)
    {
        PipelineDesc p;
        p.vertexShader = vs;
        p.fragmentShader = fs;
        p.vertexBuffers[0] = {24, StepMode::Vertex};
        p.vertexBufferCount = 1;
        p.attributes[0] = {0, 0, VertexFormat::Float2, 0};
        p.attributes[1] = {1, 0, VertexFormat::Float4, 8};
        p.attributeCount = 2;
        p.colorFormats[0] = Format::RGBA8Unorm;
        return p;
    }
}
