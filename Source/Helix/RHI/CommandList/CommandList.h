#pragma once

#include <Helix/Core/Containers/Array.h>
#include <Helix/RHI/Descriptors/Descriptors.h>

#include <cstring>

namespace helix::rhi
{
    enum class CommandType : u8
    {
        BeginRenderPass,
        EndRenderPass,
        SetViewport,
        SetScissor,
        SetPipeline,
        SetVertexBuffer,
        SetIndexBuffer,
        SetUniformBuffer,
        SetTexture,
        Draw,
        DrawIndexed,
        UpdateBuffer,
        CopyBuffer,
        CopyTextureToBuffer,
        SetStorageBuffer,
        SetStorageTexture,
        Dispatch,
        DispatchIndirect,
        DrawIndirect,
        DrawIndexedIndirect,
        BeginGpuScope,
        EndGpuScope,
        ResolveTexture,
        CopyTexture,
        GenerateMips,
    };

    struct CommandHeader
    {
        CommandType type;
        u8 pad;
        u16 size;
    };

    struct CmdBeginRenderPass
    {
        RenderPassDesc desc;
    };

    struct CmdSetViewport
    {
        Viewport viewport;
    };

    struct CmdSetScissor
    {
        ScissorRect rect;
    };

    struct CmdSetPipeline
    {
        PipelineHandle pipeline;
    };

    struct CmdSetVertexBuffer
    {
        u32 slot;
        BufferHandle buffer;
        u32 offset;
    };

    struct CmdSetIndexBuffer
    {
        BufferHandle buffer;
        u32 offset;
        IndexType type;
    };

    struct CmdSetUniformBuffer
    {
        u32 slot;
        BufferHandle buffer;
        u32 offset;
        u32 size;
    };

    struct CmdSetTexture
    {
        u32 slot;
        TextureHandle texture;
        SamplerHandle sampler;
    };

    struct CmdDraw
    {
        u32 vertexCount;
        u32 instanceCount;
        u32 firstVertex;
        u32 firstInstance;
    };

    struct CmdDrawIndexed
    {
        u32 indexCount;
        u32 instanceCount;
        u32 firstIndex;
        i32 baseVertex;
        u32 firstInstance;
    };

    struct CmdUpdateBuffer
    {
        BufferHandle buffer;
        u32 offset;
        u32 size;
    };

    struct CmdCopyBuffer
    {
        BufferHandle source;
        u32 sourceOffset;
        BufferHandle destination;
        u32 destinationOffset;
        u32 size;
    };

    struct CmdCopyTextureToBuffer
    {
        TextureHandle texture;
        u32 mipLevel;
        BufferHandle buffer;
        u32 offset;
    };

    struct CmdSetStorageBuffer
    {
        u32 slot;
        BufferHandle buffer;
        StorageAccess access;
    };

    struct CmdSetStorageTexture
    {
        u32 slot;
        TextureHandle texture;
        u32 mipLevel;
    };

    struct CmdDispatch
    {
        u32 x, y, z;
    };

    struct CmdIndirect
    {
        BufferHandle buffer;
        u32 offset;
        u32 drawCount;
        u32 stride;
    };

    struct CmdBeginGpuScope
    {
        const char* name;
    };

    struct CmdResolveTexture
    {
        TextureHandle source;
        TextureHandle destination;
    };

    struct CmdCopyTexture
    {
        TextureHandle source;
        u32 sourceMip, sourceLayer;
        TextureHandle destination;
        u32 destinationMip, destinationLayer;
    };

    struct CmdGenerateMips
    {
        TextureHandle texture;
    };

    // Commands are recorded as a compact byte stream on any thread and translated by the device on submit.
    class CommandList
    {
    public:
        explicit CommandList(Allocator& allocator = HeapAllocator()) : bytes_(allocator) {}

        void Reset()
        {
            bytes_.Clear();
            transient_.Clear();
        }

        struct TransientRange
        {
            u32 begin, end, generation;
        };
        // Ranges of transient memory this list reads, merged where adjacent; the device uploads exactly these on submit.
        Span<const TransientRange> TransientRanges() const { return transient_; }
        bool Empty() const { return bytes_.Empty(); }
        Span<const u8> Bytes() const { return bytes_; }

        void BeginRenderPass(const RenderPassDesc& desc) { Push(CommandType::BeginRenderPass, CmdBeginRenderPass{desc}); }
        void EndRenderPass() { Header(CommandType::EndRenderPass, 0); }
        void SetViewport(const Viewport& viewport) { Push(CommandType::SetViewport, CmdSetViewport{viewport}); }
        void SetScissor(const ScissorRect& rect) { Push(CommandType::SetScissor, CmdSetScissor{rect}); }
        void SetPipeline(PipelineHandle pipeline) { Push(CommandType::SetPipeline, CmdSetPipeline{pipeline}); }

        void SetVertexBuffer(u32 slot, BufferHandle buffer, u32 offset = 0)
        {
            Push(CommandType::SetVertexBuffer, CmdSetVertexBuffer{slot, buffer, offset});
        }

        void SetIndexBuffer(BufferHandle buffer, IndexType type, u32 offset = 0)
        {
            Push(CommandType::SetIndexBuffer, CmdSetIndexBuffer{buffer, offset, type});
        }

        void SetUniformBuffer(u32 slot, BufferHandle buffer, u32 offset, u32 size)
        {
            Push(CommandType::SetUniformBuffer, CmdSetUniformBuffer{slot, buffer, offset, size});
        }

        // Transient memory must be bound through this overload so the list knows which ranges it uses.
        void SetUniformBuffer(u32 slot, const TransientAllocation& a)
        {
            u32 end = a.offset + Max(a.span, a.size);
            if (transient_.Empty() || a.offset > transient_.Back().end || a.offset < transient_.Back().begin || a.generation != transient_.Back().generation)
                transient_.Push({a.offset, end, a.generation});
            else if (end > transient_.Back().end)
                transient_.Back().end = end;
            SetUniformBuffer(slot, a.buffer, a.offset, a.size);
        }

        void SetTexture(u32 slot, TextureHandle texture, SamplerHandle sampler)
        {
            Push(CommandType::SetTexture, CmdSetTexture{slot, texture, sampler});
        }

        void Draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0)
        {
            Push(CommandType::Draw, CmdDraw{vertexCount, instanceCount, firstVertex, firstInstance});
        }

        void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0, i32 baseVertex = 0, u32 firstInstance = 0)
        {
            Push(CommandType::DrawIndexed, CmdDrawIndexed{indexCount, instanceCount, firstIndex, baseVertex, firstInstance});
        }

        void UpdateBuffer(BufferHandle buffer, u32 offset, const void* data, u32 size)
        {
            HX_ASSERT(size <= 0xFFFF - sizeof(CmdUpdateBuffer) - 8);
            u8* p = Header(CommandType::UpdateBuffer, u32(sizeof(CmdUpdateBuffer) + AlignUp(size, 8)));
            CmdUpdateBuffer c{buffer, offset, size};
            std::memcpy(p, &c, sizeof(c));
            std::memcpy(p + sizeof(c), data, size);
        }

        void CopyBuffer(BufferHandle source, u32 sourceOffset, BufferHandle destination, u32 destinationOffset, u32 size)
        {
            Push(CommandType::CopyBuffer, CmdCopyBuffer{source, sourceOffset, destination, destinationOffset, size});
        }

        void CopyTextureToBuffer(TextureHandle texture, u32 mipLevel, BufferHandle buffer, u32 offset = 0)
        {
            Push(CommandType::CopyTextureToBuffer, CmdCopyTextureToBuffer{texture, mipLevel, buffer, offset});
        }

        // Whole buffer as a storage buffer for every stage; read-write binding is for compute only.
        void SetStorageBuffer(u32 slot, BufferHandle buffer, StorageAccess access = StorageAccess::ReadWrite)
        {
            Push(CommandType::SetStorageBuffer, CmdSetStorageBuffer{slot, buffer, access});
        }

        void SetStorageTexture(u32 slot, TextureHandle texture, u32 mipLevel = 0)
        {
            Push(CommandType::SetStorageTexture, CmdSetStorageTexture{slot, texture, mipLevel});
        }

        void Dispatch(u32 x, u32 y = 1, u32 z = 1) { Push(CommandType::Dispatch, CmdDispatch{x, y, z}); }
        void DispatchIndirect(BufferHandle args, u32 offset = 0) { Push(CommandType::DispatchIndirect, CmdIndirect{args, offset, 1, 0}); }

        void DrawIndirect(BufferHandle args, u32 offset = 0, u32 drawCount = 1, u32 stride = sizeof(DrawIndirectArgs))
        {
            Push(CommandType::DrawIndirect, CmdIndirect{args, offset, drawCount, stride});
        }

        void DrawIndexedIndirect(BufferHandle args, u32 offset = 0, u32 drawCount = 1, u32 stride = sizeof(DrawIndexedIndirectArgs))
        {
            Push(CommandType::DrawIndexedIndirect, CmdIndirect{args, offset, drawCount, stride});
        }

        // GPU timer scope; the name must stay valid for the lifetime of the device (a string literal).
        void BeginGpuScope(const char* name) { Push(CommandType::BeginGpuScope, CmdBeginGpuScope{name}); }
        void EndGpuScope() { Header(CommandType::EndGpuScope, 0); }

        // Outside render passes. A multisampled texture into a single sampled one of the same size and format (color).
        void ResolveTexture(TextureHandle source, TextureHandle destination) { Push(CommandType::ResolveTexture, CmdResolveTexture{source, destination}); }
        // One whole subresource (mip and layer, a cube's face is a layer) into another of the same size and format.
        void CopyTexture(TextureHandle source, u32 sourceMip, u32 sourceLayer, TextureHandle destination, u32 destinationMip, u32 destinationLayer)
        {
            Push(CommandType::CopyTexture, CmdCopyTexture{source, sourceMip, sourceLayer, destination, destinationMip, destinationLayer});
        }
        // The mips below the first from it, of a texture that is a render target and sampled.
        void GenerateMips(TextureHandle texture) { Push(CommandType::GenerateMips, CmdGenerateMips{texture}); }

        template <typename F>
        void ForEach(F&& f) const
        {
            usize at = 0;
            while (at < bytes_.Size())
            {
                CommandHeader h;
                std::memcpy(&h, bytes_.Data() + at, sizeof(h));
                f(h.type, bytes_.Data() + at + sizeof(h));
                at += sizeof(h) + h.size;
            }
        }

    private:
        u8* Header(CommandType type, u32 size)
        {
            u8* p = bytes_.AppendUninitialized(sizeof(CommandHeader) + size);
            CommandHeader h{type, 0, u16(size)};
            std::memcpy(p, &h, sizeof(h));
            return p + sizeof(h);
        }

        template <typename T>
        void Push(CommandType type, const T& value)
        {
            static_assert(sizeof(T) % 4 == 0 || alignof(T) <= 4);
            std::memcpy(Header(type, u32(AlignUp(sizeof(T), 8))), &value, sizeof(T));
        }

        Array<u8> bytes_;
        Array<TransientRange> transient_;
    };

    template <typename T>
    const T& CommandData(const u8* data)
    {
        return *reinterpret_cast<const T*>(data);
    }
}
