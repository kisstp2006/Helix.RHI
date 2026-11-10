#pragma once

#include <helix/core/base/types.h>
#include <helix/core/strings/string_view.h>

namespace helix::rhi
{
    enum class Backend : u8
    {
        D3D11,
        OpenGL,
        OpenGLES,
        WebGL2,
    };

    template <typename Tag>
    struct Handle
    {
        u32 value = 0;
        bool IsValid() const { return value != 0; }
        bool operator==(const Handle&) const = default;
    };

    using BufferHandle = Handle<struct BufferTag>;
    using TextureHandle = Handle<struct TextureTag>;
    using SamplerHandle = Handle<struct SamplerTag>;
    using ShaderHandle = Handle<struct ShaderTag>;
    using PipelineHandle = Handle<struct PipelineTag>;

    enum class Format : u8
    {
        Unknown,
        R8Unorm,
        RG8Unorm,
        RGBA8Unorm,
        RGBA8Srgb,
        BGRA8Unorm,
        BGRA8Srgb,
        R16Float,
        RG16Float,
        RGBA16Float,
        R32Float,
        RG32Float,
        RGB32Float,
        RGBA32Float,
        R32Uint,
        RG32Uint,
        RGBA32Uint,
        RGB10A2Unorm,
        RG11B10Float,
        D16Unorm,
        D24UnormS8Uint,
        D32Float,
        BC1Unorm,
        BC3Unorm,
        BC5Unorm,
        BC7Unorm,
        Count,
    };

    bool IsDepthFormat(Format format);
    bool IsCompressedFormat(Format format);
    u32 FormatBytesPerPixel(Format format);

    enum BufferUsage : u32
    {
        BufferVertex = 1,
        BufferIndex = 2,
        BufferUniform = 4,
        BufferStorage = 8,
        BufferIndirect = 16,
    };

    enum class MemoryType : u8
    {
        GpuOnly,
        Upload,
        Readback,
    };

    struct BufferDesc
    {
        u64 size = 0;
        u32 usage = 0;
        MemoryType memory = MemoryType::GpuOnly;
        u32 structureStride = 0;
        const void* initialData = nullptr;
        StringView debugName;
    };

    enum class TextureType : u8
    {
        Texture2D,
        Texture2DArray,
        Texture3D,
        TextureCube,
    };

    enum TextureUsage : u32
    {
        TextureSampled = 1,
        TextureRenderTarget = 2,
        TextureDepthStencil = 4,
        TextureStorage = 8,
    };

    struct TextureDesc
    {
        TextureType type = TextureType::Texture2D;
        Format format = Format::RGBA8Unorm;
        u32 width = 1;
        u32 height = 1;
        u32 depthOrLayers = 1;
        u32 mipLevels = 1;
        u32 sampleCount = 1;
        u32 usage = TextureSampled;
        const void* initialData = nullptr;
        StringView debugName;
    };

    enum class Filter : u8
    {
        Nearest,
        Linear,
    };

    enum class AddressMode : u8
    {
        Repeat,
        MirroredRepeat,
        ClampToEdge,
        ClampToBorder,
    };

    enum class CompareFunction : u8
    {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };

    struct SamplerDesc
    {
        Filter minFilter = Filter::Linear;
        Filter magFilter = Filter::Linear;
        Filter mipFilter = Filter::Linear;
        AddressMode addressU = AddressMode::Repeat;
        AddressMode addressV = AddressMode::Repeat;
        AddressMode addressW = AddressMode::Repeat;
        u32 maxAnisotropy = 1;
        bool compare = false;
        CompareFunction compareFunction = CompareFunction::LessEqual;
        f32 minLod = 0.0f;
        f32 maxLod = 1000.0f;
        f32 lodBias = 0.0f;
    };

    enum class ShaderStage : u8
    {
        Vertex,
        Fragment,
        Compute,
    };

    enum class BindingKind : u8
    {
        UniformBuffer,
        Texture,
        StorageBuffer,
    };

    // Ties a name used by GLSL to the slot the commands bind; D3D11 uses the registers compiled into the bytecode.
    struct ShaderBinding
    {
        StringView name;
        BindingKind kind;
        u32 slot;
    };

    struct ShaderDesc
    {
        ShaderStage stage = ShaderStage::Vertex;
        const void* bytecode = nullptr;
        usize bytecodeSize = 0;
        StringView source;
        const ShaderBinding* bindings = nullptr;
        u32 bindingCount = 0;
        StringView debugName;
    };

    enum class VertexFormat : u8
    {
        Float1,
        Float2,
        Float3,
        Float4,
        UByte4Norm,
        UInt1,
    };

    enum class StepMode : u8
    {
        Vertex,
        Instance,
    };

    struct VertexAttribute
    {
        u32 location = 0;
        u32 bufferSlot = 0;
        VertexFormat format = VertexFormat::Float3;
        u32 offset = 0;
    };

    struct VertexBufferLayout
    {
        u32 stride = 0;
        StepMode step = StepMode::Vertex;
    };

    enum class PrimitiveTopology : u8
    {
        TriangleList,
        TriangleStrip,
        LineList,
        LineStrip,
        PointList,
    };

    enum class CullMode : u8
    {
        None,
        Front,
        Back,
    };

    enum class BlendFactor : u8
    {
        Zero,
        One,
        SrcColor,
        OneMinusSrcColor,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstColor,
        OneMinusDstColor,
        DstAlpha,
        OneMinusDstAlpha,
    };

    enum class BlendOp : u8
    {
        Add,
        Subtract,
        ReverseSubtract,
        Min,
        Max,
    };

    struct BlendState
    {
        bool enabled = false;
        BlendFactor srcColor = BlendFactor::One;
        BlendFactor dstColor = BlendFactor::Zero;
        BlendOp colorOp = BlendOp::Add;
        BlendFactor srcAlpha = BlendFactor::One;
        BlendFactor dstAlpha = BlendFactor::Zero;
        BlendOp alphaOp = BlendOp::Add;
        u8 writeMask = 0xF;
    };

    inline constexpr u32 kMaxColorTargets = 8;
    inline constexpr u32 kMaxVertexBuffers = 8;
    inline constexpr u32 kMaxVertexAttributes = 16;
    inline constexpr u32 kMaxUniformSlots = 12;
    inline constexpr u32 kMaxTextureSlots = 16;

    struct PipelineDesc
    {
        ShaderHandle vertexShader;
        ShaderHandle fragmentShader;
        VertexBufferLayout vertexBuffers[kMaxVertexBuffers]{};
        u32 vertexBufferCount = 0;
        VertexAttribute attributes[kMaxVertexAttributes]{};
        u32 attributeCount = 0;
        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        CullMode cull = CullMode::None;
        bool frontCounterClockwise = false;
        bool wireframe = false;
        bool depthTest = false;
        bool depthWrite = false;
        CompareFunction depthCompare = CompareFunction::LessEqual;
        BlendState blend[kMaxColorTargets]{};
        Format colorFormats[kMaxColorTargets]{};
        u32 colorTargetCount = 1;
        Format depthFormat = Format::Unknown;
        u32 sampleCount = 1;
        StringView debugName;
    };

    enum class LoadOp : u8
    {
        Load,
        Clear,
        DontCare,
    };

    struct ColorAttachment
    {
        TextureHandle texture;
        u32 mipLevel = 0;
        u32 layer = 0;
        LoadOp load = LoadOp::Clear;
        f32 clearColor[4] = {0, 0, 0, 1};
    };

    struct DepthAttachment
    {
        TextureHandle texture;
        LoadOp load = LoadOp::Clear;
        f32 clearDepth = 1.0f;
        u8 clearStencil = 0;
    };

    // No color texture and no depth texture means the swap chain back buffer.
    struct RenderPassDesc
    {
        ColorAttachment colors[kMaxColorTargets]{};
        u32 colorCount = 1;
        DepthAttachment depth{};
    };

    struct Viewport
    {
        f32 x = 0, y = 0, width = 0, height = 0, minDepth = 0, maxDepth = 1;
    };

    struct ScissorRect
    {
        i32 x = 0, y = 0;
        u32 width = 0, height = 0;
    };

    enum class IndexType : u8
    {
        U16,
        U32,
    };

    struct DeviceCaps
    {
        Backend backend = Backend::D3D11;
        u32 apiMajor = 0;
        u32 apiMinor = 0;
        bool compute = false;
        bool storageBuffers = false;
        bool multiDrawIndirect = false;
        bool bindlessTextures = false;
        bool baseInstance = false;
        bool uniformBufferOffsets = false;
        bool persistentMapping = false;
        bool clipControl = false;
        bool textureCompressionBC = false;
        bool textureCompressionETC2 = false;
        bool textureCompressionASTC = false;
        u32 uniformBufferAlignment = 256;
        u32 maxTextureSize = 4096;
        char renderer[128] = {};
    };

    struct TransientAllocation
    {
        BufferHandle buffer;
        u32 offset = 0;
        u32 size = 0;
        void* data = nullptr;
    };
}
