#pragma once

#include <helix/platform/window.h>
#include <helix/rhi/command_list.h>

// Put once in an executable to ask hybrid graphics laptops for the discrete GPU (also for OpenGL).
#if HX_PLATFORM_WINDOWS
#define HX_RHI_PREFER_DISCRETE_GPU()                                                \
    extern "C" __declspec(dllexport) unsigned long NvOptimusEnablement = 1;         \
    extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1
#else
#define HX_RHI_PREFER_DISCRETE_GPU() static_assert(true)
#endif

namespace helix::rhi
{
    struct DeviceDesc
    {
        Backend backend = Backend::D3D11;
        // Optional for D3D11 and desktop GL (a hidden window or pbuffer is used), required for WebGL 2 and Android.
        platform::NativeWindowHandle window{};
        // D3D11 adapter index as listed by AdapterName; -1 lets the system choose.
        i32 adapter = -1;
        bool debug = false;
        bool vsync = true;
        u32 transientBufferSize = 8u << 20;
    };

    class Device
    {
    public:
        static Device* Create(const DeviceDesc& desc);
        static void Destroy(Device* device);
        static bool IsBackendAvailable(Backend backend);
        // Hardware adapters of a backend; false when the index is past the end.
        static bool AdapterName(Backend backend, u32 index, char* out, usize size);

        virtual ~Device() = default;
        virtual const DeviceCaps& Caps() const = 0;

        virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
        virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
        virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;
        virtual ShaderHandle CreateShader(const ShaderDesc& desc) = 0;
        virtual PipelineHandle CreatePipeline(const PipelineDesc& desc) = 0;
        virtual void Destroy(BufferHandle handle) = 0;
        virtual void Destroy(TextureHandle handle) = 0;
        virtual void Destroy(SamplerHandle handle) = 0;
        virtual void Destroy(ShaderHandle handle) = 0;
        virtual void Destroy(PipelineHandle handle) = 0;

        // Per-frame upload memory for uniform data; valid until the frame that allocated it has been executed by the GPU.
        virtual TransientAllocation AllocateTransient(u32 size) = 0;

        virtual void BeginFrame() = 0;
        virtual void Submit(const CommandList& commands) = 0;
        virtual void EndFrame() = 0;
        virtual void Present() = 0;
        virtual void Resize(u32 width, u32 height) = 0;
        virtual void WaitIdle() = 0;

        // Blocking read of a whole mip level as tightly packed rows, top row first.
        virtual bool ReadTexture(TextureHandle texture, u32 mipLevel, void* out, usize size) = 0;
        virtual bool ReadBuffer(BufferHandle buffer, u32 offset, void* out, u32 size) = 0;

    protected:
        virtual void DestroySelf() = 0;
    };
}
