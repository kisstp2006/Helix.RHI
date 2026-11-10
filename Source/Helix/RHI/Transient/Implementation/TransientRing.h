#pragma once

#include <Helix/Core/Containers/Array.h>
#include <Helix/RHI/Descriptors/Descriptors.h>

#include <atomic>

namespace helix::rhi
{
    namespace detail
    {
        // The part of a ring the current thread owns; one per thread is enough because a thread records for one device.
        struct TransientChunk
        {
            u64 ring = 0;
            u64 generation = 0;
            u32 epoch = 0;
            u32 cursor = 0;
            u32 end = 0;
        };

        inline thread_local TransientChunk transientChunk;
    }

    // Lock free ring of transient uniform memory shared by the backends. Positions are virtual (they only grow); the
    // physical offset is position % size and an allocation never crosses the end of the ring.
    // Threads take a chunk with one compare-and-swap and then allocate from it without touching shared state, so the
    // allocations of one thread stay contiguous.
    class TransientRing
    {
    public:
        // `limited`: allocations stop at the limit set by the owner (memory the GPU may still read lies beyond it).
        void Init(u8* memory, u32 size, u32 alignment, bool limited)
        {
            static std::atomic<u64> nextId{1};
            id_ = nextId.fetch_add(1, std::memory_order_relaxed);
            memory_ = memory;
            size_ = size;
            alignment_ = alignment;
            limited_ = limited;
            chunk_ = Max(alignment, Min<u32>(64u << 10, size / 16 / alignment * alignment));
            limit_.store(size, std::memory_order_relaxed);
        }

        u32 Size() const { return size_; }
        u64 Head() const { return head_.load(std::memory_order_acquire); }
        u64 Generation() const { return Head() / size_; }
        u64 Limit() const { return limit_.load(std::memory_order_acquire); }
        void SetLimit(u64 position) { limit_.store(position, std::memory_order_release); }
        // Makes every thread drop its current chunk, so later allocations land after the present head.
        void NextEpoch() { epoch_.fetch_add(1, std::memory_order_release); }

        // Any thread. False only for a limited ring without free space; the owner then releases memory or waits.
        bool TryAllocate(u32 size, BufferHandle buffer, TransientAllocation& out)
        {
            u32 aligned = u32(AlignUp(Max(size, 1u), alignment_));
            Chunk& c = detail::transientChunk;
            u32 epoch = epoch_.load(std::memory_order_acquire);
            bool usable = c.ring == id_ && c.epoch == epoch && c.generation == Generation() && c.cursor + aligned <= c.end;
            if (!usable)
            {
                u64 position;
                bool direct = aligned >= chunk_;
                if (!Take(direct ? aligned : chunk_, position))
                    return false;
                u32 offset = u32(position % size_);
                if (direct)
                {
                    out = {buffer, offset, size, memory_ + offset, aligned, u32(position / size_)};
                    return true;
                }
                c = {id_, position / size_, epoch, offset, offset + chunk_};
            }
            out = {buffer, c.cursor, size, memory_ + c.cursor, aligned, u32(c.generation)};
            c.cursor += aligned;
            return true;
        }

    private:
        using Chunk = detail::TransientChunk;

        bool Take(u32 bytes, u64& position)
        {
            u64 head = head_.load(std::memory_order_relaxed);
            for (;;)
            {
                u32 offset = u32(head % size_);
                u64 start = offset + bytes > size_ ? head + (size_ - offset) : head;
                if (limited_ && start + bytes > limit_.load(std::memory_order_acquire))
                    return false;
                if (head_.compare_exchange_weak(head, start + bytes, std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    position = start;
                    return true;
                }
            }
        }

        u64 id_ = 0;
        u8* memory_ = nullptr;
        u32 size_ = 1;
        u32 alignment_ = 256;
        u32 chunk_ = 256;
        bool limited_ = false;
        std::atomic<u64> head_{0};
        std::atomic<u64> limit_{0};
        std::atomic<u32> epoch_{0};
    };

    // For backends that copy the ring to the GPU buffer: remembers which byte ranges the current buffer contents hold
    // and from which wrap of the ring, so every submit uploads exactly the ranges its commands use and none twice.
    class TransientUploads
    {
    public:
        // True when [begin, end) overlaps bytes the buffer holds from another wrap of the ring: the GPU may still read
        // those, so the buffer must be renewed (discard or orphan) and Clear called before uploading.
        bool Conflicts(u32 begin, u32 end, u32 generation) const
        {
            for (usize i = FirstOverlap(begin); i < ranges_.Size() && ranges_[i].begin < end; i++)
                if (ranges_[i].generation != generation)
                    return true;
            return false;
        }

        void Clear() { ranges_.Clear(); }

        // Calls copy(begin, end) for the parts of [begin, end) that are not in the buffer yet.
        template <typename F>
        void Add(u32 begin, u32 end, u32 generation, F&& copy)
        {
            if (!ranges_.Empty() && begin >= ranges_.Back().begin && end <= ranges_.Back().end)
                return;
            usize first = FirstOverlap(begin), last = first;
            u32 cursor = begin, mergedBegin = begin, mergedEnd = end;
            for (; last < ranges_.Size() && ranges_[last].begin < end; last++)
            {
                const Range& r = ranges_[last];
                if (cursor < r.begin)
                    copy(cursor, r.begin);
                cursor = Max(cursor, r.end);
                mergedBegin = Min(mergedBegin, r.begin);
                mergedEnd = Max(mergedEnd, r.end);
            }
            if (cursor < end)
                copy(cursor, end);
            if (last == first)
                ranges_.Insert(first, Range{mergedBegin, mergedEnd, generation});
            else
            {
                ranges_[first] = {mergedBegin, mergedEnd, generation};
                for (usize i = first + 1; i < last; i++)
                    ranges_.RemoveAt(first + 1);
            }
        }

    private:
        struct Range
        {
            u32 begin, end, generation;
        };

        // Ranges are sorted and disjoint; returns the first one that ends after `begin`.
        usize FirstOverlap(u32 begin) const
        {
            usize first = 0, count = ranges_.Size();
            while (count > 0)
            {
                usize half = count / 2;
                if (ranges_[first + half].end <= begin)
                {
                    first += half + 1;
                    count -= half + 1;
                }
                else
                    count = half;
            }
            return first;
        }

        Array<Range> ranges_;
    };

}
