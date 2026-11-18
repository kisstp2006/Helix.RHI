#pragma once

#include <Helix/Platform/Window/Window.h>
#include <Helix/RHI/Bindless/Bindless.h>
#include <Helix/RHI/CommandList/CommandList.h>

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
        // D3D11: the WARP software rasterizer instead of a hardware adapter.
        bool warp = false;
        // OpenGL and OpenGL ES: the newest version the context may have, major * 10 + minor (33: an OpenGL 3.3 core
        // context, as the oldest devices a pipeline supports have); 0: the newest the driver gives.
        u32 maxGLVersion = 0;
        bool debug = false;
        bool vsync = true;
        // The depth buffer of the swap chain (render passes to the back buffer use it); Unknown for none. OpenGL's
        // default framebuffer always has 24 bit depth and 8 bit stencil.
        Format swapChainDepthFormat = Format::Unknown;
        u32 transientBufferSize = 8u << 20;
        // Size of the bindless buffer heap, allocated on the first bindless buffer.
        u32 bindlessBufferHeapSize = 64u << 20;
        // A device thread owns the backend: submits, destroys and presents are queued and return at once, creations and
        // reads wait for it (calls stay in order). Ignored where threads are not supported (the Web).
        bool threaded = false;
        // Present waits while more frames than this are queued for the device thread.
        u32 maxQueuedFrames = 1;
    };

    // A swap chain of another window: its back buffer in the device's swap chain format, a depth buffer of the
    // device's swap chain depth format.
    struct SwapChainDesc
    {
        platform::NativeWindowHandle window{};
        bool vsync = true;
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
        // A device thread owns the backend (DeviceDesc::threaded).
        virtual bool Threaded() const { return false; }

        virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
        virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
        virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;
        virtual ShaderHandle CreateShader(const ShaderDesc& desc) = 0;
        virtual PipelineHandle CreatePipeline(const PipelineDesc& desc) = 0;
        virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
        virtual void Destroy(BufferHandle handle) = 0;
        virtual void Destroy(TextureHandle handle) = 0;
        virtual void Destroy(SamplerHandle handle) = 0;
        virtual void Destroy(ShaderHandle handle) = 0;
        virtual void Destroy(PipelineHandle handle) = 0;

        // Returns a bindless index for a sampled texture and sampler, kInvalidBindlessIndex when unsupported.
        virtual u32 RegisterBindless(TextureHandle texture, SamplerHandle sampler) = 0;
        virtual void UnregisterBindless(u32 index) = 0;
        // Index of a bindless buffer in the buffer table, kInvalidBindlessIndex when the buffer is not in the heap.
        virtual u32 RegisterBindlessBuffer(BufferHandle buffer) = 0;
        virtual void UnregisterBindlessBuffer(u32 index) = 0;

        // Per-frame upload memory for uniform data; valid until the frame that allocated it has been executed by the GPU.
        // Safe to call from any thread, without locks. When several threads record, bracket the frame with BeginFrame
        // and EndFrame and submit its command lists in between; one frame may use at most transientBufferSize bytes.
        virtual TransientAllocation AllocateTransient(u32 size) = 0;

        // Scopes of the newest frame whose timestamps are ready (frames end at EndFrame); returns the count written.
        // While a profiler capture runs the scopes are also recorded on the profiler track "GPU".
        virtual u32 ReadGpuTimings(GpuTiming* out, u32 max) = 0;

        virtual void BeginFrame() = 0;
        virtual void Submit(const CommandList& commands) = 0;
        virtual void EndFrame() = 0;
        virtual void Present() = 0;
        virtual void Resize(u32 width, u32 height) = 0;

        // Swap chains of other windows (D3D11, desktop OpenGL; invalid where a device has one window only). A render
        // pass draws to one by RenderPassDesc::swapChain; Present and Resize take it like the device window's.
        virtual SwapChainHandle CreateSwapChain(const SwapChainDesc& desc) = 0;
        virtual void Destroy(SwapChainHandle handle) = 0;
        virtual void Resize(SwapChainHandle handle, u32 width, u32 height) = 0;
        virtual void Present(SwapChainHandle handle) = 0;
        // The size of a swap chain's back buffer (0 by 0 for none).
        virtual void SwapChainSize(SwapChainHandle handle, u32& width, u32& height) = 0;
        virtual void WaitIdle() = 0;

        // Blocking read of a whole mip level as tightly packed rows, top row first.
        virtual bool ReadTexture(TextureHandle texture, u32 mipLevel, void* out, usize size) = 0;
        virtual bool ReadBuffer(BufferHandle buffer, u32 offset, void* out, u32 size) = 0;

    protected:
        virtual void DestroySelf() = 0;
    };
}
