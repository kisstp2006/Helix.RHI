#pragma once

#include <Helix/Core/Basics/Types.h>
#include <Helix/Core/Strings/StringView.h>

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
        R32Sint,
        RG32Sint,
        RGBA32Sint,
        RGB10A2Unorm,
        RG11B10Float,
        D16Unorm,
        D24UnormS8Uint,
        D32Float,
        BC1Unorm,
        BC3Unorm,
        BC5Unorm,
        BC7Unorm,
        BC2Unorm,
        BC4Unorm,
        BC4Snorm,
        BC5Snorm,
        BC6HUfloat,
        BC6HSfloat,
        ETC2RGB8,
        ETC2RGB8A1,
        ETC2RGBA8,
        EACR11,
        EACR11Snorm,
        EACRG11,
        EACRG11Snorm,
        ASTC4x4,
        ASTC5x4,
        ASTC5x5,
        ASTC6x5,
        ASTC6x6,
        ASTC8x5,
        ASTC8x6,
        ASTC8x8,
        ASTC10x5,
        ASTC10x6,
        ASTC10x8,
        ASTC10x10,
        ASTC12x10,
        ASTC12x12,
        Count,
    };

    bool IsDepthFormat(Format format);
    bool IsCompressedFormat(Format format);
    u32 FormatBytesPerPixel(Format format);

    // Texels per block and bytes per block; uncompressed formats are 1 x 1 blocks of one texel.
    struct FormatBlockInfo
    {
        u32 width = 1, height = 1, bytes = 0;
    };
    FormatBlockInfo FormatBlock(Format format);
    // Bytes of one 2D image of the format, whole blocks for compressed formats.
    u64 FormatImageBytes(Format format, u32 width, u32 height);
    // Bytes of a row of blocks (of texels for uncompressed formats).
    u32 FormatRowBytes(Format format, u32 width);

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
        // Suballocates from the bindless buffer heap (GPU only memory; vertex, index, storage and indirect use; raw views on D3D11).
        bool bindless = false;
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
        // Allocates the texture where the backend can address it by bindless index (a texture array pool on D3D11).
        bool bindless = false;
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
        StorageTexture,
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
    // Storage buffers and storage textures share slots 0 to 6; GL binding 7 holds the bindless table.
    // D3D11: read-write resources use register u<slot>, read-only storage buffers t<kStorageD3D11ReadOnlyRegister + slot>.
    inline constexpr u32 kMaxStorageSlots = 7;
    inline constexpr u32 kStorageD3D11ReadOnlyRegister = 16;

    enum class StorageAccess : u8
    {
        ReadOnly,
        ReadWrite,
    };

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

    struct ComputePipelineDesc
    {
        ShaderHandle computeShader;
        StringView debugName;
    };

    // Argument layouts of indirect commands, identical on every backend.
    struct DrawIndirectArgs
    {
        u32 vertexCount, instanceCount, firstVertex, firstInstance;
    };

    struct DrawIndexedIndirectArgs
    {
        u32 indexCount, instanceCount, firstIndex;
        i32 baseVertex;
        u32 firstInstance;
    };

    struct DispatchIndirectArgs
    {
        u32 x, y, z;
    };

    // A GPU scope of the most recently resolved frame; times are relative to the first scope of that frame.
    struct GpuTiming
    {
        const char* name = nullptr;
        u32 depth = 0;
        f64 beginMs = 0;
        f64 durationMs = 0;
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
        u32 mipLevel = 0;
        u32 layer = 0;
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
        bool drawIndirect = false;
        bool multiDrawIndirect = false;
        bool gpuTimers = false;
        bool bindlessTextures = false;
        bool bindlessEmulated = false;
        u32 maxBindlessTextures = 0;
        bool bindlessBuffers = false;
        bool baseInstance = false;
        bool uniformBufferOffsets = false;
        bool persistentMapping = false;
        bool clipControl = false;
        // BC1 to BC3; BC4 and BC5 (RGTC); BC6H and BC7 (BPTC).
        bool textureCompressionBC = false;
        bool textureCompressionRGTC = false;
        bool textureCompressionBPTC = false;
        // ETC2 and EAC.
        bool textureCompressionETC2 = false;
        bool textureCompressionASTC = false;
        u32 uniformBufferAlignment = 256;
        u32 maxTextureSize = 4096;
        // The most samples of a multisampled render target (1: none).
        u32 maxSamples = 1;
        // The formats of the swap chain's back buffer and depth buffer (Unknown without a window or depth).
        Format swapChainFormat = Format::Unknown;
        Format swapChainDepthFormat = Format::Unknown;
        char renderer[128] = {};
    };

    struct TransientAllocation
    {
        BufferHandle buffer;
        u32 offset = 0;
        u32 size = 0;
        void* data = nullptr;
        // Bytes reserved in the ring including alignment padding, and how often the ring had wrapped at that time.
        u32 span = 0;
        u32 generation = 0;
    };
}
