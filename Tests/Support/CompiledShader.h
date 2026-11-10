#pragma once

#include <Helix/RHI/Device/Device.h>

namespace helix::rhi::testing
{
    // A test shader in every form the backends take. The instances live in CompiledShaders.h, which the shader compiler
    // of the engine generates from C# sources; this library sits below that compiler and cannot run it in its own build.
    struct CompiledShader
    {
        ShaderStage stage;
        const char* glsl;
        const char* essl;
        const unsigned char* dxbc;
        usize dxbcSize;
        // Names in the GLSL sources and the slots they stand for; Direct3D has the slots in the bytecode.
        const ShaderBinding* bindings;
        u32 bindingCount;
    };

    inline ShaderHandle MakeShader(Device& device, const CompiledShader& shader)
    {
        ShaderDesc desc;
        desc.stage = shader.stage;
        desc.bindings = shader.bindings;
        desc.bindingCount = shader.bindingCount;
        Backend backend = device.Caps().backend;
        if (backend == Backend::D3D11)
        {
            desc.bytecode = shader.dxbc;
            desc.bytecodeSize = shader.dxbcSize;
        }
        else
            desc.source = backend == Backend::OpenGLES || backend == Backend::WebGL2 ? shader.essl : shader.glsl;
        return device.CreateShader(desc);
    }
}
