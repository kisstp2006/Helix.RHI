#pragma once

#include <Helix/Core/Basics/Types.h>

namespace helix::rhi
{
    // Bindless textures: Device::RegisterBindless returns a dense index that shaders use on every backend.
    //
    // OpenGL 4.5 with ARB_bindless_texture:
    //   layout(std430, binding = kBindlessGLTableBinding) readonly buffer HelixBindless { uvec2 helixHandles[]; };
    //   texture(sampler2D(helixHandles[index]), uv)
    //
    // Direct3D 11 (emulated, textures live in Texture2DArray pools):
    //   ByteAddressBuffer HelixBindless : register(t127), four bytes per entry = pool | slice << 5 | sampler << 16
    //   Texture2DArray HelixPool<N> : register(t96 + N), SamplerState HelixSampler<M> : register(s8 + M)
    //
    // Bindless buffers: buffers created with BufferDesc::bindless live in one raw heap per device; RegisterBindlessBuffer
    // returns an index into a table of byte (offset, size) pairs.
    //   OpenGL: layout(std430, binding = 8) readonly buffer HelixBufferTable { uvec2 helixBuffers[]; };
    //           layout(std430, binding = 9) buffer HelixBufferHeap { uint helixHeap[]; };
    //   D3D11:  ByteAddressBuffer HelixBufferTable : register(t125), eight bytes per entry;
    //           ByteAddressBuffer HelixBufferHeap : register(t126) in graphics shaders, RWByteAddressBuffer : register(u7) in compute
    inline constexpr u32 kInvalidBindlessIndex = 0xFFFFFFFFu;
    inline constexpr u32 kBindlessGLBufferTableBinding = 8;
    inline constexpr u32 kBindlessGLBufferHeapBinding = 9;
    inline constexpr u32 kBindlessD3D11BufferTableRegister = 125;
    inline constexpr u32 kBindlessD3D11BufferHeapRegister = 126;
    inline constexpr u32 kBindlessD3D11BufferHeapUavRegister = 7;
    inline constexpr u32 kBindlessBufferAlignment = 256;

    struct BindlessBufferEntry
    {
        u32 offset = 0;
        u32 size = 0;
    };
    inline constexpr u32 kBindlessGLTableBinding = 7;
    inline constexpr u32 kBindlessD3D11TableRegister = 127;
    inline constexpr u32 kBindlessD3D11PoolRegister = 96;
    inline constexpr u32 kBindlessD3D11MaxPools = 16;
    inline constexpr u32 kBindlessD3D11SamplerRegister = 8;
    inline constexpr u32 kBindlessD3D11MaxSamplers = 8;
    inline constexpr u32 kBindlessD3D11MaxSlices = 2048;

    constexpr u32 PackD3D11BindlessEntry(u32 pool, u32 slice, u32 sampler) { return pool | (slice << 5) | (sampler << 16); }
}
