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
    //   StructuredBuffer<uint> HelixBindless : register(t127), entry = pool | slice << 5 | sampler << 16
    //   Texture2DArray HelixPool<N> : register(t96 + N), SamplerState HelixSampler<M> : register(s8 + M)
    inline constexpr u32 kInvalidBindlessIndex = 0xFFFFFFFFu;
    inline constexpr u32 kBindlessGLTableBinding = 7;
    inline constexpr u32 kBindlessD3D11TableRegister = 127;
    inline constexpr u32 kBindlessD3D11PoolRegister = 96;
    inline constexpr u32 kBindlessD3D11MaxPools = 16;
    inline constexpr u32 kBindlessD3D11SamplerRegister = 8;
    inline constexpr u32 kBindlessD3D11MaxSamplers = 8;
    inline constexpr u32 kBindlessD3D11MaxSlices = 2048;

    constexpr u32 PackD3D11BindlessEntry(u32 pool, u32 slice, u32 sampler) { return pool | (slice << 5) | (sampler << 16); }
}
