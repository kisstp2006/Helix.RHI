#include <Helix/RHI/Device/Device.h>

#include <Helix/Platform/Threading/Thread.h>
#include <Helix/RHI/Transient/Implementation/TransientRing.h>

#include <cstring>
#include <utility>

// The threaded mode (DeviceDesc::threaded): a device thread owns the backend device. Every call goes through one queue
// in order: submits, destroys, frame brackets, presents and resizes are queued and return at once; creations, reads,
// registrations and waits run on the device thread while the caller waits for their result. So a destroy queued after
// a submit runs after it, and the backend sees one thread as before. Present blocks while more than
// DeviceDesc::maxQueuedFrames frames are waiting. Transient memory comes from a ring of this layer: the device thread
// copies what a list binds into the backend's ring when it submits it, and the memory is reused once the device thread
// has taken it (a submit outside a frame, or a frame's end).
namespace helix::rhi
{
    Device* CreateBackendDevice(const DeviceDesc& desc);

    namespace
    {
        enum class TaskKind : u8
        {
            Call,
            Submit,
            DestroyBuffer,
            DestroyTexture,
            DestroySampler,
            DestroyShader,
            DestroyPipeline,
            BeginFrame,
            EndFrame,
            Present,
            Resize,
            DestroySwapChain,
            ResizeSwapChain,
            PresentSwapChain,
            Quit,
        };

        struct Task
        {
            TaskKind kind = TaskKind::Call;
            u32 a = 0, b = 0, c = 0;
            // The transient ring's head when queued, and whether taking this task frees the memory before it.
            u64 mark = 0;
            bool frees = false;
            CommandList* list = nullptr;
            void (*call)(void* context, Device& device) = nullptr;
            void* context = nullptr;
        };

        class ThreadedDevice final : public Device
        {
        public:
            explicit ThreadedDevice(const DeviceDesc& desc) : desc_(desc) { desc_.threaded = false; }

            bool Start()
            {
                if (!thread_.Start({.function = [](void* self) { static_cast<ThreadedDevice*>(self)->Run(); }, .user = this, .name = "helix-render"}))
                    return false;
                // The backend device is made on the device thread (a GL context belongs to the thread that made it).
                platform::ScopedLock lock(mutex_);
                while (!started_)
                    changed_.Wait(mutex_);
                if (!inner_)
                    return false;
                ringMemory_.Resize(desc_.transientBufferSize);
                ring_.Init(ringMemory_.Data(), desc_.transientBufferSize, Max(256u, inner_->Caps().uniformBufferAlignment), true);
                return true;
            }

            const DeviceCaps& Caps() const override { return inner_->Caps(); }
            bool Threaded() const override { return true; }

            BufferHandle CreateBuffer(const BufferDesc& desc) override
            {
                return Call([&](Device& d) { return d.CreateBuffer(desc); });
            }
            TextureHandle CreateTexture(const TextureDesc& desc) override
            {
                return Call([&](Device& d) { return d.CreateTexture(desc); });
            }
            SamplerHandle CreateSampler(const SamplerDesc& desc) override
            {
                return Call([&](Device& d) { return d.CreateSampler(desc); });
            }
            ShaderHandle CreateShader(const ShaderDesc& desc) override
            {
                return Call([&](Device& d) { return d.CreateShader(desc); });
            }
            PipelineHandle CreatePipeline(const PipelineDesc& desc) override
            {
                return Call([&](Device& d) { return d.CreatePipeline(desc); });
            }
            PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override
            {
                return Call([&](Device& d) { return d.CreateComputePipeline(desc); });
            }

            void Destroy(BufferHandle h) override { Queue({.kind = TaskKind::DestroyBuffer, .a = h.value}); }
            void Destroy(TextureHandle h) override { Queue({.kind = TaskKind::DestroyTexture, .a = h.value}); }
            void Destroy(SamplerHandle h) override { Queue({.kind = TaskKind::DestroySampler, .a = h.value}); }
            void Destroy(ShaderHandle h) override { Queue({.kind = TaskKind::DestroyShader, .a = h.value}); }
            void Destroy(PipelineHandle h) override { Queue({.kind = TaskKind::DestroyPipeline, .a = h.value}); }

            u32 RegisterBindless(TextureHandle texture, SamplerHandle sampler) override
            {
                return Call([&](Device& d) { return d.RegisterBindless(texture, sampler); });
            }
            void UnregisterBindless(u32 index) override
            {
                Call([&](Device& d) {
                    d.UnregisterBindless(index);
                    return 0;
                });
            }
            u32 RegisterBindlessBuffer(BufferHandle buffer) override
            {
                return Call([&](Device& d) { return d.RegisterBindlessBuffer(buffer); });
            }
            void UnregisterBindlessBuffer(u32 index) override
            {
                Call([&](Device& d) {
                    d.UnregisterBindlessBuffer(index);
                    return 0;
                });
            }

            // Any thread; waits while the device thread still has to take the memory it would reuse.
            TransientAllocation AllocateTransient(u32 size) override
            {
                TransientAllocation a{};
                if (size > ring_.Size())
                    return a;
                while (!ring_.TryAllocate(size, kRingBuffer, a))
                {
                    platform::ScopedLock lock(mutex_);
                    u64 limit = ring_.Limit();
                    if (ring_.TryAllocate(size, kRingBuffer, a))
                        break;
                    // More than one frame's budget in flight: stop protecting the oldest memory instead of hanging.
                    if (!changed_.WaitFor(mutex_, 1.0) && ring_.Limit() == limit)
                        ring_.SetLimit(ring_.Head() + ring_.Size());
                }
                return a;
            }

            u32 ReadGpuTimings(GpuTiming* out, u32 max) override
            {
                return Call([&](Device& d) { return d.ReadGpuTimings(out, max); });
            }

            void BeginFrame() override
            {
                inFrame_ = true;
                ring_.NextEpoch();
                Queue({.kind = TaskKind::BeginFrame});
            }

            void Submit(const CommandList& commands) override
            {
                CommandList* copy = nullptr;
                {
                    platform::ScopedLock lock(mutex_);
                    if (!spareLists_.Empty())
                    {
                        copy = spareLists_.Back();
                        spareLists_.Pop();
                    }
                }
                if (!copy)
                    copy = new CommandList();
                copy->Assign(commands);
                // Outside frames the threads' chunks end here, so later allocations lie after the mark (else a chunk could
                // hand out memory below it once the ring has wrapped).
                if (!inFrame_)
                    ring_.NextEpoch();
                Queue({.kind = TaskKind::Submit, .mark = ring_.Head(), .frees = !inFrame_, .list = copy});
            }

            void EndFrame() override
            {
                inFrame_ = false;
                ring_.NextEpoch();
                Queue({.kind = TaskKind::EndFrame, .mark = ring_.Head(), .frees = true});
            }

            void Present() override
            {
                platform::ScopedLock lock(mutex_);
                tasks_.Push({.kind = TaskKind::Present});
                queuedFrames_++;
                changed_.NotifyAll();
                while (queuedFrames_ > desc_.maxQueuedFrames)
                    changed_.Wait(mutex_);
            }

            void Resize(u32 width, u32 height) override { Queue({.kind = TaskKind::Resize, .a = width, .b = height}); }

            SwapChainHandle CreateSwapChain(const SwapChainDesc& desc) override
            {
                return Call([&](Device& d) { return d.CreateSwapChain(desc); });
            }
            void Destroy(SwapChainHandle handle) override { Queue({.kind = TaskKind::DestroySwapChain, .a = handle.value}); }
            void Resize(SwapChainHandle handle, u32 width, u32 height) override
            {
                Queue({.kind = TaskKind::ResizeSwapChain, .a = handle.value, .b = width, .c = height});
            }
            // Another window's present: in order with the frame's work, not counted as a frame.
            void Present(SwapChainHandle handle) override { Queue({.kind = TaskKind::PresentSwapChain, .a = handle.value}); }
            void SwapChainSize(SwapChainHandle handle, u32& width, u32& height) override
            {
                Call([&](Device& d) {
                    d.SwapChainSize(handle, width, height);
                    return 0;
                });
            }

            void WaitIdle() override
            {
                Call([&](Device& d) {
                    d.WaitIdle();
                    return 0;
                });
            }

            bool ReadTexture(TextureHandle texture, u32 mipLevel, void* out, usize size) override
            {
                return Call([&](Device& d) { return d.ReadTexture(texture, mipLevel, out, size); });
            }
            bool ReadBuffer(BufferHandle buffer, u32 offset, void* out, u32 size) override
            {
                return Call([&](Device& d) { return d.ReadBuffer(buffer, offset, out, size); });
            }

        protected:
            void DestroySelf() override
            {
                Queue({.kind = TaskKind::Quit});
                thread_.Join();
                for (CommandList* l : spareLists_)
                    delete l;
                delete this;
            }

        private:
            template <typename F>
            auto Call(F&& f) -> decltype(f(std::declval<Device&>()))
            {
                using R = decltype(f(*inner_));
                struct Context
                {
                    F* f;
                    R result{};
                    bool done = false;
                } context{&f};
                Task t;
                t.kind = TaskKind::Call;
                t.context = &context;
                t.call = [](void* c, Device& d) {
                    auto* ctx = static_cast<Context*>(c);
                    ctx->result = (*ctx->f)(d);
                };
                platform::ScopedLock lock(mutex_);
                t.b = ++callSerial_;
                tasks_.Push(t);
                changed_.NotifyAll();
                while (finishedCall_ < t.b)
                    changed_.Wait(mutex_);
                return context.result;
            }

            void Queue(const Task& t)
            {
                platform::ScopedLock lock(mutex_);
                tasks_.Push(t);
                changed_.NotifyAll();
            }

            void Run()
            {
                Device* inner = CreateBackendDevice(desc_);
                {
                    platform::ScopedLock lock(mutex_);
                    inner_ = inner;
                    started_ = true;
                    changed_.NotifyAll();
                }
                if (!inner)
                    return;
                Array<Task> batch;
                for (bool quit = false; !quit;)
                {
                    {
                        platform::ScopedLock lock(mutex_);
                        while (tasks_.Empty())
                            changed_.Wait(mutex_);
                        batch = Move(tasks_);
                        tasks_ = Array<Task>();
                    }
                    for (const Task& t : batch)
                    {
                        Device& d = *inner;
                        switch (t.kind)
                        {
                        case TaskKind::Call: t.call(t.context, d); break;
                        case TaskKind::Submit: d.Submit(Convert(d, *t.list)); break;
                        case TaskKind::DestroyBuffer: d.Destroy(BufferHandle{t.a}); break;
                        case TaskKind::DestroyTexture: d.Destroy(TextureHandle{t.a}); break;
                        case TaskKind::DestroySampler: d.Destroy(SamplerHandle{t.a}); break;
                        case TaskKind::DestroyShader: d.Destroy(ShaderHandle{t.a}); break;
                        case TaskKind::DestroyPipeline: d.Destroy(PipelineHandle{t.a}); break;
                        case TaskKind::BeginFrame: d.BeginFrame(); break;
                        case TaskKind::EndFrame: d.EndFrame(); break;
                        case TaskKind::Present: d.Present(); break;
                        case TaskKind::Resize: d.Resize(t.a, t.b); break;
                        case TaskKind::DestroySwapChain: d.Destroy(SwapChainHandle{t.a}); break;
                        case TaskKind::ResizeSwapChain: d.Resize(SwapChainHandle{t.a}, t.b, t.c); break;
                        case TaskKind::PresentSwapChain: d.Present(SwapChainHandle{t.a}); break;
                        case TaskKind::Quit: quit = true; break;
                        }
                        platform::ScopedLock lock(mutex_);
                        if (t.frees && t.mark + ring_.Size() > ring_.Limit())
                        {
                            ring_.SetLimit(t.mark + ring_.Size());
                            changed_.NotifyAll();
                        }
                        if (t.kind == TaskKind::Call)
                            finishedCall_ = t.b;
                        else if (t.kind == TaskKind::Present)
                            queuedFrames_--;
                        else if (t.kind == TaskKind::Submit)
                        {
                            t.list->Reset();
                            spareLists_.Push(t.list);
                        }
                        if (t.kind == TaskKind::Call || t.kind == TaskKind::Present)
                            changed_.NotifyAll();
                        if (quit)
                            break;
                    }
                    batch.Clear();
                }
                Device::Destroy(inner);
            }

            // The list as the backend takes it: transient bindings copied into the backend's ring, the rest as recorded.
            const CommandList& Convert(Device& d, const CommandList& list)
            {
                converted_.Reset();
                list.ForEachSized([&](CommandType type, const u8* data, u32 size) {
                    if (type == CommandType::SetUniformBuffer)
                    {
                        const CmdSetUniformBuffer& c = CommandData<CmdSetUniformBuffer>(data);
                        if (c.buffer == kRingBuffer)
                        {
                            TransientAllocation a = d.AllocateTransient(c.size);
                            if (a.data)
                                std::memcpy(a.data, ringMemory_.Data() + c.offset, c.size);
                            converted_.SetUniformBuffer(c.slot, a);
                            return;
                        }
                    }
                    converted_.AppendRecorded(type, data, size);
                });
                return converted_;
            }

            // The handle transient allocations of this layer carry (no backend makes it: its index is the last).
            static constexpr BufferHandle kRingBuffer{0xFFFFFFFFu};

            DeviceDesc desc_;
            Array<u8> ringMemory_;
            TransientRing ring_;
            CommandList converted_;
            bool inFrame_ = false;
            Device* inner_ = nullptr;
            platform::Thread thread_;
            platform::Mutex mutex_;
            platform::ConditionVariable changed_;
            Array<Task> tasks_;
            Array<CommandList*> spareLists_;
            u32 callSerial_ = 0;
            u32 finishedCall_ = 0;
            u32 queuedFrames_ = 0;
            bool started_ = false;
        };
    }

    Device* CreateThreadedDevice(const DeviceDesc& desc)
    {
        auto* d = new ThreadedDevice(desc);
        if (d->Start())
            return d;
        Device::Destroy(d);
        return nullptr;
    }
}
