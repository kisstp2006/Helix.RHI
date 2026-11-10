#pragma once

#include <Helix/Core/Containers/Array.h>
#include <Helix/RHI/Bindless/Bindless.h>

namespace helix::rhi
{
    // Dense index table; freed indices are reused, the whole array is uploaded when dirty.
    template <typename Entry>
    class BindlessTable
    {
    public:
        u32 Add(Entry entry)
        {
            dirty_ = true;
            if (!free_.Empty())
            {
                u32 index = free_.Back();
                free_.Pop();
                entries_[index] = entry;
                return index;
            }
            entries_.Push(entry);
            return u32(entries_.Size() - 1);
        }

        bool Remove(u32 index, Entry& out)
        {
            if (index >= entries_.Size() || free_.Contains(index))
                return false;
            out = entries_[index];
            entries_[index] = Entry{};
            free_.Push(index);
            dirty_ = true;
            return true;
        }

        const Entry* Data() const { return entries_.Data(); }
        u32 Size() const { return u32(entries_.Size()); }
        bool Empty() const { return entries_.Size() == free_.Size(); }
        bool Dirty() const { return dirty_; }
        void ClearDirty() { dirty_ = false; }

    private:
        Array<Entry> entries_;
        Array<u32> free_;
        bool dirty_ = false;
    };
}
