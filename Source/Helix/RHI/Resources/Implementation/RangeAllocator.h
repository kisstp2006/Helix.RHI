#pragma once

#include <Helix/Core/Containers/Array.h>

namespace helix::rhi
{
    // First fit allocator over a fixed byte range; free ranges stay sorted and are merged with their neighbours.
    class RangeAllocator
    {
    public:
        static constexpr u64 kInvalid = ~0ull;

        void Reset(u64 size)
        {
            free_.Clear();
            if (size)
                free_.Push({0, size});
        }

        u64 Allocate(u64 size, u64 alignment)
        {
            size = AlignUp(size ? size : 1, alignment);
            for (usize i = 0; i < free_.Size(); i++)
            {
                Range& r = free_[i];
                u64 start = AlignUp(r.offset, alignment);
                if (start + size > r.offset + r.size)
                    continue;
                u64 end = r.offset + r.size;
                u64 head = start - r.offset;
                if (head)
                {
                    r.size = head;
                    if (start + size < end)
                        free_.Insert(i + 1, {start + size, end - start - size});
                }
                else if (start + size < end)
                {
                    r.offset = start + size;
                    r.size = end - r.offset;
                }
                else
                    free_.RemoveAt(i);
                return start;
            }
            return kInvalid;
        }

        void Free(u64 offset, u64 size, u64 alignment)
        {
            size = AlignUp(size ? size : 1, alignment);
            usize i = 0;
            while (i < free_.Size() && free_[i].offset < offset)
                i++;
            free_.Insert(i, {offset, size});
            if (i + 1 < free_.Size() && free_[i].offset + free_[i].size == free_[i + 1].offset)
            {
                free_[i].size += free_[i + 1].size;
                free_.RemoveAt(i + 1);
            }
            if (i > 0 && free_[i - 1].offset + free_[i - 1].size == free_[i].offset)
            {
                free_[i - 1].size += free_[i].size;
                free_.RemoveAt(i);
            }
        }

        u64 FreeBytes() const
        {
            u64 n = 0;
            for (const Range& r : free_)
                n += r.size;
            return n;
        }

        usize FreeRangeCount() const { return free_.Size(); }

    private:
        struct Range
        {
            u64 offset, size;
        };
        Array<Range> free_;
    };
}
