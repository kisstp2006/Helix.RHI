#pragma once

#include <Helix/Core/Containers/Array.h>
#include <Helix/RHI/Descriptors/Descriptors.h>

namespace helix::rhi
{
    // Handle value: generation in the upper 12 bits, slot index (1 based) in the lower 20 bits.
    template <typename T, typename H>
    class Pool
    {
    public:
        static constexpr u32 kIndexBits = 20;
        static constexpr u32 kIndexMask = (1u << kIndexBits) - 1;

        H Add(T&& value)
        {
            u32 index;
            if (!free_.Empty())
            {
                index = free_.Back();
                free_.Pop();
                items_[index] = Move(value);
            }
            else
            {
                if (items_.Empty())
                {
                    items_.Emplace();
                    generations_.Push(0);
                }
                index = u32(items_.Size());
                items_.Push(Move(value));
                generations_.Push(1);
            }
            live_++;
            return H{(generations_[index] << kIndexBits) | index};
        }

        T* Get(H handle)
        {
            u32 index = handle.value & kIndexMask;
            if (!handle.value || index >= items_.Size() || generations_[index] != handle.value >> kIndexBits)
                return nullptr;
            return &items_[index];
        }

        bool Remove(H handle, T& out)
        {
            T* item = Get(handle);
            if (!item)
                return false;
            u32 index = handle.value & kIndexMask;
            out = Move(*item);
            *item = T();
            generations_[index] = (generations_[index] + 1) & 0xFFF;
            if (!generations_[index])
                generations_[index] = 1;
            free_.Push(index);
            live_--;
            return true;
        }

        template <typename F>
        void ForEach(F&& f)
        {
            for (usize i = 1; i < items_.Size(); i++)
                if (!free_.Contains(u32(i)))
                    f(items_[i]);
        }

        u32 LiveCount() const { return live_; }

    private:
        Array<T> items_;
        Array<u32> generations_;
        Array<u32> free_;
        u32 live_ = 0;
    };
}
