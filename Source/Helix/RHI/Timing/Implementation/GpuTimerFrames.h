#pragma once

#include <Helix/Core/Containers/Array.h>
#include <Helix/Core/Containers/HashMap.h>
#include <Helix/Platform/Threading/Thread.h>
#include <Helix/Profiler/Profiler.h>
#include <Helix/RHI/Descriptors/Descriptors.h>

namespace helix::rhi
{
    // Backend independent bookkeeping of timestamp queries: a ring of frames, scope nesting, conversion and profiler output.
    class GpuTimerFrames
    {
    public:
        static constexpr u32 kFrames = 4;
        static constexpr u32 kMaxScopes = 1024;

        struct Scope
        {
            const char* name;
            u32 depth;
            u32 begin;
            u32 end;
        };

        struct Frame
        {
            Array<Scope> scopes;
            Array<u32> open;
            u32 queries = 0;
            u64 cpuStart = 0;
            bool pending = false;
        };

        u32 CurrentIndex() const { return current_; }
        Frame& At(u32 index) { return frames_[index]; }
        bool Recording() const { return !frames_[current_].pending; }

        // Query index for the start of a scope, -1 when this frame does not record.
        i32 Begin(const char* name)
        {
            Frame& f = frames_[current_];
            if (f.pending || f.scopes.Size() >= kMaxScopes)
                return -1;
            if (f.queries == 0)
                f.cpuStart = profiler::CaptureTicks();
            f.open.Push(u32(f.scopes.Size()));
            f.scopes.Push({name, u32(f.open.Size() - 1), f.queries, 0});
            return i32(f.queries++);
        }

        i32 End()
        {
            Frame& f = frames_[current_];
            if (f.pending || f.open.Empty())
                return -1;
            f.scopes[f.open.Back()].end = f.queries;
            f.open.Pop();
            return i32(f.queries++);
        }

        // Closes the current frame; returns true when it holds queries the backend must finish (for example a disjoint query).
        bool Close()
        {
            Frame& f = frames_[current_];
            if (f.pending)
                return false;
            while (!f.open.Empty())
                End();
            f.pending = f.queries > 0;
            return f.pending;
        }

        // Moves to the next frame slot unless that slot still waits for its results.
        void Advance()
        {
            u32 next = (current_ + 1) % kFrames;
            if (!frames_[next].pending)
                current_ = next;
        }

        // Oldest pending frame first, so results stay in order.
        template <typename F>
        void ForEachPending(F&& fetch)
        {
            for (u32 i = 1; i <= kFrames; i++)
            {
                u32 index = (current_ + i) % kFrames;
                Frame& f = frames_[index];
                if (f.pending && !fetch(index, f))
                    return;
            }
        }

        // Called by the backend with the frame's raw timestamps (ticks of `frequency` per second); invalid drops the frame.
        void Resolve(Frame& f, const u64* ticks, u64 frequency, bool valid)
        {
            if (valid && frequency)
            {
                latest_.Clear();
                f64 toMs = 1000.0 / f64(frequency);
                for (const Scope& s : f.scopes)
                    latest_.Push({s.name, s.depth, f64(ticks[s.begin] - ticks[0]) * toMs, f64(ticks[s.end] - ticks[s.begin]) * toMs});
                if (profiler::IsCapturing())
                    Record(f, ticks, frequency);
            }
            f.scopes.Clear();
            f.open.Clear();
            f.queries = 0;
            f.pending = false;
        }

        u32 Read(GpuTiming* out, u32 max) const
        {
            u32 n = Min(max, u32(latest_.Size()));
            for (u32 i = 0; i < n; i++)
                out[i] = latest_[i];
            return n;
        }

    private:
        void Record(const Frame& f, const u64* ticks, u64 frequency)
        {
            Array<i32> events;
            events.Resize(f.queries, 0);
            for (usize i = 0; i < f.scopes.Size(); i++)
            {
                events[f.scopes[i].begin] = i32(i + 1);
                events[f.scopes[i].end] = -i32(i + 1);
            }
            f64 scale = f64(profiler::CaptureTicksPerSecond()) / f64(frequency);
            for (u32 q = 0; q < f.queries; q++)
            {
                if (!events[q])
                    continue;
                const Scope& s = f.scopes[usize(events[q] > 0 ? events[q] : -events[q]) - 1];
                profiler::Marker& marker = MarkerFor(s.name);
                u64 cpu = f.cpuStart + u64(f64(ticks[q] - ticks[0]) * scale);
                profiler::RecordTrackEvent(0, "GPU", marker, cpu, events[q] > 0 ? profiler::EventKind::Begin : profiler::EventKind::End);
            }
        }

        // The profiler keeps marker pointers, so markers live for the whole process.
        static profiler::Marker& MarkerFor(const char* name)
        {
            static platform::Mutex mutex;
            static HashMap<uptr, profiler::Marker*>* markers = New<HashMap<uptr, profiler::Marker*>>(HeapAllocator());
            platform::ScopedLock lock(mutex);
            profiler::Marker*& marker = (*markers)[reinterpret_cast<uptr>(name)];
            if (!marker)
                marker = New<profiler::Marker>(HeapAllocator(), name);
            return *marker;
        }

        Frame frames_[kFrames];
        u32 current_ = 0;
        Array<GpuTiming> latest_;
    };
}
