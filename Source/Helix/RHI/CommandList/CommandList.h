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

    // Commands are recorded as a compact byte stream on any thread and translated by the device on submit.
    class CommandList
    {
    public:
        explicit CommandList(Allocator& allocator = HeapAllocator()) : bytes_(allocator) {}

        void Reset() { bytes_.Clear(); }
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

        void SetUniformBuffer(u32 slot, const TransientAllocation& a) { SetUniformBuffer(slot, a.buffer, a.offset, a.size); }

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
            usize at = bytes_.Size();
            bytes_.Resize(at + sizeof(CommandHeader) + size);
            CommandHeader h{type, 0, u16(size)};
            std::memcpy(bytes_.Data() + at, &h, sizeof(h));
            return bytes_.Data() + at + sizeof(h);
        }

        template <typename T>
        void Push(CommandType type, const T& value)
        {
            static_assert(sizeof(T) % 4 == 0 || alignof(T) <= 4);
            std::memcpy(Header(type, u32(AlignUp(sizeof(T), 8))), &value, sizeof(T));
        }

        Array<u8> bytes_;
    };

    template <typename T>
    const T& CommandData(const u8* data)
    {
        return *reinterpret_cast<const T*>(data);
    }
}
