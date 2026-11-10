#pragma once

#include <helix/core/containers/array.h>
#include <helix/core/strings/format.h>
#include <helix/rhi/device.h>

#if HX_PLATFORM_WINDOWS
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>

namespace helix::rhi::testing
{
    // Test shaders: HLSL for D3D11 (compiled by the operating system compiler, tests only) and GLSL for the GL family.
    struct ShaderPair
    {
        const char* hlslVertex;
        const char* hlslPixel;
        const char* glslVertex;
        const char* glslFragment;
    };

    struct Blob
    {
        Array<u8> bytes;
    };

#if HX_PLATFORM_WINDOWS
    inline bool CompileHlsl(const char* source, const char* target, Blob& out)
    {
        struct ID3DBlobLike
        {
            struct Vtbl
            {
                long(__stdcall* QueryInterface)(void*, const void*, void**);
                unsigned long(__stdcall* AddRef)(void*);
                unsigned long(__stdcall* Release)(void*);
                void*(__stdcall* GetBufferPointer)(void*);
                usize(__stdcall* GetBufferSize)(void*);
            } const* vtbl;
        };
        using CompileFn = long(__stdcall*)(const void*, usize, const char*, const void*, void*, const char*, const char*, unsigned, unsigned,
                                           ID3DBlobLike**, ID3DBlobLike**);
        static HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!lib)
            return false;
        auto compile = reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(lib, "D3DCompile")));
        ID3DBlobLike* code = nullptr;
        ID3DBlobLike* errors = nullptr;
        long hr = compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors);
        if (errors)
        {
            std::printf("    hlsl: %s\n", static_cast<const char*>(errors->vtbl->GetBufferPointer(errors)));
            errors->vtbl->Release(errors);
        }
        if (hr < 0 || !code)
            return false;
        usize n = code->vtbl->GetBufferSize(code);
        out.bytes.Resize(n);
        std::memcpy(out.bytes.Data(), code->vtbl->GetBufferPointer(code), n);
        code->vtbl->Release(code);
        return true;
    }
#endif

    inline String GlslSource(const DeviceCaps& caps, const char* body)
    {
        bool es = caps.backend == Backend::OpenGLES || caps.backend == Backend::WebGL2;
        return helix::Format("{}{}", es ? "#version 300 es\nprecision highp float;\nprecision highp int;\n" : "#version 330 core\n", body);
    }

    inline ShaderHandle MakeShader(Device& device, ShaderStage stage, const ShaderPair& pair, const ShaderBinding* bindings = nullptr, u32 bindingCount = 0)
    {
        ShaderDesc desc;
        desc.stage = stage;
        desc.bindings = bindings;
        desc.bindingCount = bindingCount;
        if (device.Caps().backend == Backend::D3D11)
        {
#if HX_PLATFORM_WINDOWS
            Blob blob;
            if (!CompileHlsl(stage == ShaderStage::Vertex ? pair.hlslVertex : pair.hlslPixel, stage == ShaderStage::Vertex ? "vs_5_0" : "ps_5_0", blob))
                return {};
            desc.bytecode = blob.bytes.Data();
            desc.bytecodeSize = blob.bytes.Size();
            return device.CreateShader(desc);
#else
            return {};
#endif
        }
        String source = GlslSource(device.Caps(), stage == ShaderStage::Vertex ? pair.glslVertex : pair.glslFragment);
        desc.source = source;
        return device.CreateShader(desc);
    }
}
