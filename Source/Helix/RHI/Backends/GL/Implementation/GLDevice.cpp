#include <Helix/RHI/Bindless/Implementation/BindlessTable.h>
#include <Helix/RHI/Resources/Implementation/RangeAllocator.h>
#include <Helix/RHI/Resources/Implementation/ResourcePool.h>
#include <Helix/RHI/Backends/GL/Implementation/GLContext.h>
#include <Helix/RHI/Timing/Implementation/GpuTimerFrames.h>
#include <Helix/RHI/Transient/Implementation/TransientRing.h>

#include <Helix/Platform/Threading/Thread.h>

#include <Helix/Core/Containers/HashMap.h>
#include <Helix/Core/Logging/Log.h>
#include <Helix/Core/Strings/String.h>
#include <Helix/GL/Loader.h>

#include <cstdio>
#include <cstring>

namespace helix::rhi
{
    namespace
    {
        // IEEE half from float, rounded to nearest even; infinities and NaN kept.
        u16 FloatToHalf(f32 value)
        {
            u32 bits;
            std::memcpy(&bits, &value, 4);
            u32 sign = (bits >> 16) & 0x8000u, exponent = (bits >> 23) & 0xFFu, mantissa = bits & 0x7FFFFFu;
            if (exponent == 0xFF)
                return u16(sign | 0x7C00u | (mantissa ? 0x200u : 0));
            i32 e = i32(exponent) - 127 + 15;
            if (e >= 31)
                return u16(sign | 0x7C00u);
            if (e <= 0)
            {
                if (e < -10)
                    return u16(sign);
                mantissa |= 0x800000u;
                u32 shift = u32(14 - e);
                u32 half = mantissa >> shift, rest = mantissa & ((1u << shift) - 1), middle = 1u << (shift - 1);
                if (rest > middle || (rest == middle && (half & 1)))
                    half++;
                return u16(sign | half);
            }
            u32 half = (u32(e) << 10) | (mantissa >> 13), rest = mantissa & 0x1FFFu;
            if (rest > 0x1000u || (rest == 0x1000u && (half & 1)))
                half++;
            return u16(sign | half);
        }
    }

    using namespace helix::gl;

    namespace
    {
        LogChannel GLLog{"RHI.GL"};

        struct GLFormat
        {
            GLenum internal, format, type;
            bool compressed;
        };

        GLFormat ToGL(Format f)
        {
            switch (f)
            {
            case Format::R8Unorm: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE, false};
            case Format::RG8Unorm: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, false};
            case Format::RGBA8Unorm:
            case Format::BGRA8Unorm: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false};
            case Format::RGBA8Srgb:
            case Format::BGRA8Srgb: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, false};
            case Format::R16Float: return {GL_R16F, GL_RED, GL_HALF_FLOAT, false};
            case Format::RG16Float: return {GL_RG16F, GL_RG, GL_HALF_FLOAT, false};
            case Format::RGBA16Float: return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, false};
            case Format::R32Float: return {GL_R32F, GL_RED, GL_FLOAT, false};
            case Format::RG32Float: return {GL_RG32F, GL_RG, GL_FLOAT, false};
            case Format::RGB32Float: return {GL_RGB32F, GL_RGB, GL_FLOAT, false};
            case Format::RGBA32Float: return {GL_RGBA32F, GL_RGBA, GL_FLOAT, false};
            case Format::R32Uint: return {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, false};
            case Format::RG32Uint: return {GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, false};
            case Format::RGBA32Uint: return {GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, false};
            case Format::R32Sint: return {GL_R32I, GL_RED_INTEGER, GL_INT, false};
            case Format::RG32Sint: return {GL_RG32I, GL_RG_INTEGER, GL_INT, false};
            case Format::RGBA32Sint: return {GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, false};
            case Format::RGB10A2Unorm: return {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, false};
            case Format::RG11B10Float: return {GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, false};
            case Format::D16Unorm: return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, false};
            case Format::D24UnormS8Uint: return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, false};
            case Format::D32Float: return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, false};
            case Format::BC1Unorm: return {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, true};
            case Format::BC3Unorm: return {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, true};
            case Format::BC5Unorm: return {GL_COMPRESSED_RG_RGTC2, 0, 0, true};
            case Format::BC7Unorm: return {GL_COMPRESSED_RGBA_BPTC_UNORM, 0, 0, true};
            case Format::BC2Unorm: return {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, true};
            case Format::BC4Unorm: return {GL_COMPRESSED_RED_RGTC1, 0, 0, true};
            case Format::BC4Snorm: return {GL_COMPRESSED_SIGNED_RED_RGTC1, 0, 0, true};
            case Format::BC5Snorm: return {GL_COMPRESSED_SIGNED_RG_RGTC2, 0, 0, true};
            case Format::BC6HUfloat: return {GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT, 0, 0, true};
            case Format::BC6HSfloat: return {GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT, 0, 0, true};
            case Format::ETC2RGB8: return {GL_COMPRESSED_RGB8_ETC2, 0, 0, true};
            case Format::ETC2RGB8A1: return {GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, 0, 0, true};
            case Format::ETC2RGBA8: return {GL_COMPRESSED_RGBA8_ETC2_EAC, 0, 0, true};
            case Format::EACR11: return {GL_COMPRESSED_R11_EAC, 0, 0, true};
            case Format::EACR11Snorm: return {GL_COMPRESSED_SIGNED_R11_EAC, 0, 0, true};
            case Format::EACRG11: return {GL_COMPRESSED_RG11_EAC, 0, 0, true};
            case Format::EACRG11Snorm: return {GL_COMPRESSED_SIGNED_RG11_EAC, 0, 0, true};
            case Format::ASTC4x4: return {GL_COMPRESSED_RGBA_ASTC_4x4, 0, 0, true};
            case Format::ASTC5x4: return {GL_COMPRESSED_RGBA_ASTC_5x4, 0, 0, true};
            case Format::ASTC5x5: return {GL_COMPRESSED_RGBA_ASTC_5x5, 0, 0, true};
            case Format::ASTC6x5: return {GL_COMPRESSED_RGBA_ASTC_6x5, 0, 0, true};
            case Format::ASTC6x6: return {GL_COMPRESSED_RGBA_ASTC_6x6, 0, 0, true};
            case Format::ASTC8x5: return {GL_COMPRESSED_RGBA_ASTC_8x5, 0, 0, true};
            case Format::ASTC8x6: return {GL_COMPRESSED_RGBA_ASTC_8x6, 0, 0, true};
            case Format::ASTC8x8: return {GL_COMPRESSED_RGBA_ASTC_8x8, 0, 0, true};
            case Format::ASTC10x5: return {GL_COMPRESSED_RGBA_ASTC_10x5, 0, 0, true};
            case Format::ASTC10x6: return {GL_COMPRESSED_RGBA_ASTC_10x6, 0, 0, true};
            case Format::ASTC10x8: return {GL_COMPRESSED_RGBA_ASTC_10x8, 0, 0, true};
            case Format::ASTC10x10: return {GL_COMPRESSED_RGBA_ASTC_10x10, 0, 0, true};
            case Format::ASTC12x10: return {GL_COMPRESSED_RGBA_ASTC_12x10, 0, 0, true};
            case Format::ASTC12x12: return {GL_COMPRESSED_RGBA_ASTC_12x12, 0, 0, true};
            default: return {0, 0, 0, false};
            }
        }

        GLenum ToCompare(CompareFunction f)
        {
            static const GLenum map[] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
            return map[int(f)];
        }

        GLenum ToBlend(BlendFactor f)
        {
            static const GLenum map[] = {GL_ZERO,      GL_ONE,         GL_SRC_COLOR,           GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                         GL_ONE_MINUS_SRC_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
            return map[int(f)];
        }

        GLenum ToBlendOp(BlendOp op)
        {
            static const GLenum map[] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
            return map[int(op)];
        }

        GLenum ToAddress(AddressMode m)
        {
            static const GLenum map[] = {GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER};
            return map[int(m)];
        }

        GLenum ToTopology(PrimitiveTopology t)
        {
            static const GLenum map[] = {GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_LINES, GL_LINE_STRIP, GL_POINTS};
            return map[int(t)];
        }

        struct VertexFormatInfo
        {
            GLint components;
            GLenum type;
            bool normalized;
            bool integer;
        };

        VertexFormatInfo ToVertex(VertexFormat f)
        {
            switch (f)
            {
            case VertexFormat::Float1: return {1, GL_FLOAT, false, false};
            case VertexFormat::Float2: return {2, GL_FLOAT, false, false};
            case VertexFormat::Float3: return {3, GL_FLOAT, false, false};
            case VertexFormat::Float4: return {4, GL_FLOAT, false, false};
            case VertexFormat::UByte4Norm: return {4, GL_UNSIGNED_BYTE, true, false};
            case VertexFormat::UInt1: return {1, GL_UNSIGNED_INT, false, true};
            }
            return {4, GL_FLOAT, false, false};
        }

        struct Buffer
        {
            GLuint id = 0;
            BufferDesc desc{};
            u32 heapOffset = 0;
            bool inHeap = false;
        };

        struct Texture
        {
            GLuint id = 0;
            GLenum target = GL_TEXTURE_2D;
            // A multisampled target is a renderbuffer (every GL and GLES version has those; it is resolved to be read).
            GLuint renderbuffer = 0;
            TextureDesc desc{};
        };

        // A framebuffer object and the textures it holds (it goes with any of them).
        struct Framebuffer
        {
            GLuint id = 0;
            u32 textures[kMaxColorTargets + 1] = {};
        };

        struct Sampler
        {
            GLuint id = 0;
        };

        struct BindingName
        {
            String name;
            BindingKind kind;
            u32 slot;
        };

        struct Shader
        {
            GLuint id = 0;
            ShaderStage stage{};
            Array<BindingName> bindings;
        };

        struct Pipeline
        {
            GLuint program = 0;
            bool compute = false;
            PipelineDesc desc{};
        };

        class GLDevice final : public Device
        {
        public:
            GLContext* context = nullptr;
            DeviceCaps caps{};
            bool vsync = true;
            Pool<Buffer, BufferHandle> buffers;
            Pool<Texture, TextureHandle> textures;
            Pool<Sampler, SamplerHandle> samplers;
            Pool<Shader, ShaderHandle> shaders;
            Pool<Pipeline, PipelineHandle> pipelines;
            HashMap<u64, Framebuffer> framebuffers;
            GLuint vao = 0;
            GLuint readFramebuffer = 0;
            GLuint blitFramebuffer = 0;

            BufferHandle transient;
            GLuint transientId = 0;
            Array<u8> transientCpu;
            u32 transientSize = 0;
            TransientRing transientRing;
            TransientUploads transientUploads;

            // Persistent coherent mapping (GL 4.4): allocations are written in place. Every submit leaves a fence; the
            // memory of a group is free again once its fence has passed. Inside BeginFrame and EndFrame all submits
            // form one group, because any of them may read what the frame allocated.
            struct TransientGroup
            {
                GLsync sync;
                u64 begin, end;
                bool open;
            };
            u8* transientMapped = nullptr;
            Array<TransientGroup> transientGroups;
            u64 transientFenced = 0;
            u64 deviceThread = 0;
            bool inFrame = false;
            bool transientWarned = false;

            BindlessTable<GLuint64> bindless;
            GLuint bindlessBuffer = 0;
            u32 bindlessCapacity = 0;

            Pipeline* pipeline = nullptr;
            PipelineHandle appliedPipeline;
            struct VertexBinding
            {
                GLuint buffer = 0;
                u32 offset = 0;
            } vertexBuffers[kMaxVertexBuffers];
            bool attributesDirty = true;
            u32 enabledAttributes = 0;
            GLenum indexType = GL_UNSIGNED_SHORT;
            u32 indexOffset = 0;
            bool offscreen = false;
            u32 targetHeight = 0;
            bool frontCounterClockwise = false;
            GLuint currentProgram = 0;
            Pipeline* computePipeline = nullptr;
            bool barrierPending = false;

            GpuTimerFrames timers;
            Array<GLuint> timestampQueries[GpuTimerFrames::kFrames];
            bool timerExt = false;

            u32 heapSize = 0;
            u32 heapAlignment = kBindlessBufferAlignment;
            GLuint heap = 0;
            RangeAllocator heapRanges;
            BindlessTable<BindlessBufferEntry> bufferTable;
            GLuint bufferTableId = 0;
            u32 bufferTableCapacity = 0;

            bool Init(const DeviceDesc& desc)
            {
                vsync = desc.vsync;
                context = CreateGLContext(desc);
                if (!context)
                    return false;
                context->SetSwapInterval(vsync ? 1 : 0);
                int major = 0, minor = 0;
                bool es = false;
                GetVersion(major, minor, es);
                es = es || context->IsES();
                auto at = [&](int ma, int mi) { return major > ma || (major == ma && minor >= mi); };
                if ((es ? !at(3, 0) : !at(3, 3)) || !api.GenVertexArrays || !api.BindBufferRange || !api.GenSamplers)
                {
                    HX_LOG_WARNING(GLLog, "{} {}.{} is below the minimum (GL 3.3 or ES 3.0)", es ? "OpenGL ES" : "OpenGL", major, minor);
                    return false;
                }
                caps.backend = desc.backend == Backend::WebGL2 ? Backend::WebGL2 : es ? Backend::OpenGLES : Backend::OpenGL;
                // The default framebuffer: RGBA8 with 24 bit depth and 8 bit stencil (the context's pixel format).
                if (desc.window.window)
                {
                    caps.swapChainFormat = Format::RGBA8Unorm;
                    caps.swapChainDepthFormat = Format::D24UnormS8Uint;
                }
                caps.apiMajor = u32(major);
                caps.apiMinor = u32(minor);
                bool web = caps.backend == Backend::WebGL2;
                caps.compute = !web && (es ? at(3, 1) : at(4, 3));
                caps.storageBuffers = caps.compute;
                caps.drawIndirect = !web && (es ? at(3, 1) : at(4, 0)) && api.DrawArraysIndirect && api.DrawElementsIndirect;
                timerExt = es && HasExtension("GL_EXT_disjoint_timer_query") && api.QueryCounterEXT && api.GetQueryObjectui64vEXT;
                caps.gpuTimers = !web && (es ? timerExt : api.QueryCounter && api.GetQueryObjectui64v && api.GetQueryObjectuiv);
                if (caps.storageBuffers)
                {
                    GLint bindings = 0, vertexBlocks = 0, fragmentBlocks = 0, ssboAlignment = 0;
                    glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &bindings);
                    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vertexBlocks);
                    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fragmentBlocks);
                    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlignment);
                    caps.bindlessBuffers = bindings > GLint(kBindlessGLBufferHeapBinding) && vertexBlocks >= 3 && fragmentBlocks >= 3;
                    heapAlignment = Max(kBindlessBufferAlignment, u32(ssboAlignment));
                }
                heapSize = desc.bindlessBufferHeapSize;
                caps.multiDrawIndirect = !es && (at(4, 3) || HasExtension("GL_ARB_multi_draw_indirect"));
                caps.bindlessTextures = HasExtension("GL_ARB_bindless_texture") && at(4, 3) && api.GetTextureSamplerHandleARB &&
                                        api.MakeTextureHandleResidentARB;
                caps.maxBindlessTextures = caps.bindlessTextures ? 1u << 20 : 0;
                caps.baseInstance = !es && at(4, 2);
                caps.uniformBufferOffsets = true;
                caps.persistentMapping = !es && (at(4, 4) || HasExtension("GL_ARB_buffer_storage"));
                caps.clipControl = api.ClipControl != nullptr && (at(4, 5) || HasExtension("GL_ARB_clip_control") || HasExtension("GL_EXT_clip_control"));
                caps.textureCompressionBC = HasExtension("GL_EXT_texture_compression_s3tc") || HasExtension("WEBGL_compressed_texture_s3tc");
                caps.textureCompressionRGTC = (!es && at(3, 0)) || HasExtension("GL_ARB_texture_compression_rgtc") || HasExtension("GL_EXT_texture_compression_rgtc");
                caps.textureCompressionBPTC = (!es && at(4, 2)) || HasExtension("GL_ARB_texture_compression_bptc") || HasExtension("GL_EXT_texture_compression_bptc");
                // ETC2 and EAC are core in OpenGL ES 3.0 and OpenGL 4.3.
                caps.textureCompressionETC2 = es || at(4, 3) || HasExtension("GL_ARB_ES3_compatibility");
                caps.textureCompressionASTC = HasExtension("GL_KHR_texture_compression_astc_ldr");
                GLint alignment = 256, maxSize = 4096;
                glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
                caps.uniformBufferAlignment = u32(Max(alignment, 16));
                caps.maxTextureSize = u32(maxSize);
                const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
                std::snprintf(caps.renderer, sizeof(caps.renderer), "%s", renderer ? renderer : "unknown");

                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                glGenFramebuffers(1, &readFramebuffer);
                glGenFramebuffers(1, &blitFramebuffer);
                GLint samples = 1;
                glGetIntegerv(GL_MAX_SAMPLES, &samples);
                caps.maxSamples = u32(Max(samples, 1));
                if (!es)
                    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

                transientSize = desc.transientBufferSize;
                glGenBuffers(1, &transientId);
                glBindBuffer(GL_UNIFORM_BUFFER, transientId);
                if (caps.persistentMapping && api.BufferStorage && api.MapBufferRange && api.FenceSync && api.ClientWaitSync)
                {
                    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                    glBufferStorage(GL_UNIFORM_BUFFER, GLsizeiptr(transientSize), nullptr, flags);
                    transientMapped = static_cast<u8*>(glMapBufferRange(GL_UNIFORM_BUFFER, 0, GLsizeiptr(transientSize), flags));
                    if (!transientMapped)
                    {
                        glDeleteBuffers(1, &transientId);
                        glGenBuffers(1, &transientId);
                        glBindBuffer(GL_UNIFORM_BUFFER, transientId);
                    }
                }
                if (!transientMapped)
                {
                    transientCpu.Resize(transientSize);
                    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(transientSize), nullptr, GL_STREAM_DRAW);
                }
                transientRing.Init(transientMapped ? transientMapped : transientCpu.Data(), transientSize, caps.uniformBufferAlignment, transientMapped != nullptr);
                deviceThread = platform::CurrentThreadId();
                transient = buffers.Add(Buffer{transientId, BufferDesc{transientSize, BufferUniform, MemoryType::Upload}});
                HX_LOG_INFO(GLLog, "{} {}.{} on {}", es ? (web ? "WebGL" : "OpenGL ES") : "OpenGL", major, minor, caps.renderer);
                return glGetError() == GL_NO_ERROR || true;
            }

            void Shutdown()
            {
                if (!context)
                    return;
                if (!vao)
                {
                    DestroyGLContext(context);
                    context = nullptr;
                    return;
                }
                for (const TransientGroup& g : transientGroups)
                    glDeleteSync(g.sync);
                if (transientMapped)
                {
                    glBindBuffer(GL_UNIFORM_BUFFER, transientId);
                    glUnmapBuffer(GL_UNIFORM_BUFFER);
                }
                buffers.ForEach([](Buffer& b) {
                    if (!b.inHeap)
                        glDeleteBuffers(1, &b.id);
                });
                if (heap)
                    glDeleteBuffers(1, &heap);
                if (bufferTableId)
                    glDeleteBuffers(1, &bufferTableId);
                textures.ForEach([](Texture& t) { glDeleteTextures(1, &t.id); });
                samplers.ForEach([](Sampler& s) { glDeleteSamplers(1, &s.id); });
                shaders.ForEach([](Shader& s) { glDeleteShader(s.id); });
                pipelines.ForEach([](Pipeline& p) { glDeleteProgram(p.program); });
                for (Array<GLuint>& queries : timestampQueries)
                    if (!queries.Empty())
                        glDeleteQueries(GLsizei(queries.Size()), queries.Data());
                for (auto e : framebuffers)
                    glDeleteFramebuffers(1, &e.value.id);
                for (u32 i = 0; i < bindless.Size(); i++)
                    if (bindless.Data()[i])
                        glMakeTextureHandleNonResidentARB(bindless.Data()[i]);
                if (bindlessBuffer)
                    glDeleteBuffers(1, &bindlessBuffer);
                glDeleteFramebuffers(1, &readFramebuffer);
                glDeleteFramebuffers(1, &blitFramebuffer);
                glDeleteVertexArrays(1, &vao);
                DestroyGLContext(context);
                context = nullptr;
            }

            const DeviceCaps& Caps() const override { return caps; }

            BufferHandle CreateHeapBuffer(const BufferDesc& desc)
            {
                if (!caps.bindlessBuffers || desc.memory != MemoryType::GpuOnly || desc.usage & BufferUniform)
                    return {};
                if (!heap)
                {
                    glGenBuffers(1, &heap);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, heap);
                    glBufferData(GL_COPY_WRITE_BUFFER, GLsizeiptr(heapSize), nullptr, GL_STATIC_DRAW);
                    heapRanges.Reset(heapSize);
                }
                u64 offset = heapRanges.Allocate(desc.size, heapAlignment);
                if (offset == RangeAllocator::kInvalid)
                    return {};
                if (desc.initialData)
                {
                    glBindBuffer(GL_COPY_WRITE_BUFFER, heap);
                    glBufferSubData(GL_COPY_WRITE_BUFFER, GLintptr(offset), GLsizeiptr(desc.size), desc.initialData);
                }
                Buffer b;
                b.id = heap;
                b.desc = desc;
                b.desc.initialData = nullptr;
                b.heapOffset = u32(offset);
                b.inHeap = true;
                return buffers.Add(Move(b));
            }

            u32 RegisterBindlessBuffer(BufferHandle handle) override
            {
                Buffer* b = buffers.Get(handle);
                if (!b || !b->inHeap)
                    return kInvalidBindlessIndex;
                return bufferTable.Add({b->heapOffset, u32(b->desc.size)});
            }

            void UnregisterBindlessBuffer(u32 index) override
            {
                BindlessBufferEntry e;
                bufferTable.Remove(index, e);
            }

            void UploadBufferTable()
            {
                if (!heap)
                    return;
                if (!bufferTableId)
                    glGenBuffers(1, &bufferTableId);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, bufferTableId);
                if (bufferTable.Dirty() || !bufferTableCapacity)
                {
                    u32 bytes = bufferTable.Size() * u32(sizeof(BindlessBufferEntry));
                    if (bytes > bufferTableCapacity || !bufferTableCapacity)
                    {
                        bufferTableCapacity = Max(bytes * 2, 4096u);
                        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(bufferTableCapacity), nullptr, GL_DYNAMIC_DRAW);
                    }
                    if (bytes)
                        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(bytes), bufferTable.Data());
                    bufferTable.ClearDirty();
                }
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindlessGLBufferTableBinding, bufferTableId);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindlessGLBufferHeapBinding, heap);
            }

            BufferHandle CreateBuffer(const BufferDesc& desc) override
            {
                if (desc.bindless)
                    return CreateHeapBuffer(desc);
                Buffer b;
                b.desc = desc;
                b.desc.initialData = nullptr;
                glGenBuffers(1, &b.id);
                // WebGL 2 fixes a buffer's kind at its first binding: index buffers are first bound as index buffers (the
                // vertex array's index binding is set again before every indexed draw).
                GLenum target = (desc.usage & BufferIndex) != 0 ? GL_ELEMENT_ARRAY_BUFFER : GL_COPY_WRITE_BUFFER;
                glBindBuffer(target, b.id);
                GLenum usage = desc.memory == MemoryType::Upload ? GL_DYNAMIC_DRAW : desc.memory == MemoryType::Readback ? GL_STREAM_READ : GL_STATIC_DRAW;
                glBufferData(target, GLsizeiptr(desc.size), desc.initialData, usage);
                return buffers.Add(Move(b));
            }

            TextureHandle CreateTexture(const TextureDesc& desc) override
            {
                Texture t;
                t.desc = desc;
                t.desc.initialData = nullptr;
                t.target = desc.type == TextureType::Texture2DArray ? GL_TEXTURE_2D_ARRAY
                         : desc.type == TextureType::Texture3D      ? GL_TEXTURE_3D
                         : desc.type == TextureType::TextureCube    ? GL_TEXTURE_CUBE_MAP
                                                                    : GL_TEXTURE_2D;
                GLFormat f = ToGL(desc.format);
                if (!f.internal)
                    return {};
                if (desc.sampleCount > 1)
                {
                    if (desc.type != TextureType::Texture2D || desc.sampleCount > caps.maxSamples)
                        return {};
                    t.target = GL_RENDERBUFFER;
                    glGenRenderbuffers(1, &t.renderbuffer);
                    glBindRenderbuffer(GL_RENDERBUFFER, t.renderbuffer);
                    glRenderbufferStorageMultisample(GL_RENDERBUFFER, GLsizei(desc.sampleCount), f.internal, GLsizei(desc.width), GLsizei(desc.height));
                    glBindRenderbuffer(GL_RENDERBUFFER, 0);
                    return textures.Add(Move(t));
                }
                glGenTextures(1, &t.id);
                glBindTexture(t.target, t.id);
                GLsizei mips = GLsizei(desc.mipLevels);
                if (t.target == GL_TEXTURE_2D || t.target == GL_TEXTURE_CUBE_MAP)
                    glTexStorage2D(t.target, mips, f.internal, GLsizei(desc.width), GLsizei(desc.height));
                else
                    glTexStorage3D(t.target, mips, f.internal, GLsizei(desc.width), GLsizei(desc.height), GLsizei(desc.depthOrLayers));
                glTexParameteri(t.target, GL_TEXTURE_MAX_LEVEL, mips - 1);
                if (desc.initialData && f.compressed)
                {
                    // Whole blocks of the first mip, one image per face or layer.
                    GLsizei bytes = GLsizei(FormatImageBytes(desc.format, desc.width, desc.height));
                    const u8* data = static_cast<const u8*>(desc.initialData);
                    if (t.target == GL_TEXTURE_2D)
                        glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(desc.width), GLsizei(desc.height), f.internal, bytes, data);
                    else if (t.target == GL_TEXTURE_CUBE_MAP)
                        for (u32 i = 0; i < 6; i++)
                            glCompressedTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, 0, 0, GLsizei(desc.width), GLsizei(desc.height), f.internal, bytes,
                                                      data + usize(bytes) * i);
                    else
                        glCompressedTexSubImage3D(t.target, 0, 0, 0, 0, GLsizei(desc.width), GLsizei(desc.height), GLsizei(desc.depthOrLayers), f.internal,
                                                  bytes * GLsizei(desc.depthOrLayers), data);
                }
                if (desc.initialData && !f.compressed)
                {
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    if (t.target == GL_TEXTURE_2D)
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(desc.width), GLsizei(desc.height), f.format, f.type, desc.initialData);
                    else if (t.target == GL_TEXTURE_CUBE_MAP)
                    {
                        usize face = usize(desc.width) * desc.height * FormatBytesPerPixel(desc.format);
                        for (u32 i = 0; i < 6; i++)
                            glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, 0, 0, GLsizei(desc.width), GLsizei(desc.height), f.format, f.type,
                                            static_cast<const u8*>(desc.initialData) + face * i);
                    }
                    else
                        glTexSubImage3D(t.target, 0, 0, 0, 0, GLsizei(desc.width), GLsizei(desc.height), GLsizei(desc.depthOrLayers), f.format, f.type,
                                        desc.initialData);
                }
                return textures.Add(Move(t));
            }

            SamplerHandle CreateSampler(const SamplerDesc& desc) override
            {
                Sampler s;
                glGenSamplers(1, &s.id);
                GLenum minFilter = desc.minFilter == Filter::Linear ? (desc.mipFilter == Filter::Linear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                                                                    : (desc.mipFilter == Filter::Linear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
                glSamplerParameteri(s.id, GL_TEXTURE_MIN_FILTER, GLint(minFilter));
                glSamplerParameteri(s.id, GL_TEXTURE_MAG_FILTER, desc.magFilter == Filter::Linear ? GL_LINEAR : GL_NEAREST);
                glSamplerParameteri(s.id, GL_TEXTURE_WRAP_S, GLint(ToAddress(desc.addressU)));
                glSamplerParameteri(s.id, GL_TEXTURE_WRAP_T, GLint(ToAddress(desc.addressV)));
                glSamplerParameteri(s.id, GL_TEXTURE_WRAP_R, GLint(ToAddress(desc.addressW)));
                glSamplerParameterf(s.id, GL_TEXTURE_MIN_LOD, desc.minLod);
                glSamplerParameterf(s.id, GL_TEXTURE_MAX_LOD, desc.maxLod);
                if (desc.compare)
                {
                    glSamplerParameteri(s.id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
                    glSamplerParameteri(s.id, GL_TEXTURE_COMPARE_FUNC, GLint(ToCompare(desc.compareFunction)));
                }
                if (desc.maxAnisotropy > 1)
                    glSamplerParameterf(s.id, GL_TEXTURE_MAX_ANISOTROPY_EXT, f32(desc.maxAnisotropy));
                glGetError();
                return samplers.Add(Move(s));
            }

            ShaderHandle CreateShader(const ShaderDesc& desc) override
            {
                if (desc.source.Empty())
                    return {};
                Shader s;
                s.stage = desc.stage;
                s.id = glCreateShader(desc.stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : desc.stage == ShaderStage::Fragment ? GL_FRAGMENT_SHADER : GL_COMPUTE_SHADER);
                const GLchar* text = desc.source.Data();
                GLint length = GLint(desc.source.Size());
                glShaderSource(s.id, 1, &text, &length);
                glCompileShader(s.id);
                GLint ok = 0;
                glGetShaderiv(s.id, GL_COMPILE_STATUS, &ok);
                if (!ok)
                {
                    char log[2048];
                    glGetShaderInfoLog(s.id, sizeof(log), nullptr, log);
                    HX_LOG_ERROR(GLLog, "shader {} failed: {}", desc.debugName, StringView(log));
                    glDeleteShader(s.id);
                    return {};
                }
                for (u32 i = 0; i < desc.bindingCount; i++)
                    s.bindings.Push({String(desc.bindings[i].name), desc.bindings[i].kind, desc.bindings[i].slot});
                return shaders.Add(Move(s));
            }

            PipelineHandle CreatePipeline(const PipelineDesc& desc) override
            {
                Shader* vs = shaders.Get(desc.vertexShader);
                Shader* fs = shaders.Get(desc.fragmentShader);
                if (!vs || !fs)
                    return {};
                Pipeline p;
                p.desc = desc;
                p.program = glCreateProgram();
                glAttachShader(p.program, vs->id);
                glAttachShader(p.program, fs->id);
                glLinkProgram(p.program);
                glDetachShader(p.program, vs->id);
                glDetachShader(p.program, fs->id);
                GLint ok = 0;
                glGetProgramiv(p.program, GL_LINK_STATUS, &ok);
                if (!ok)
                {
                    char log[2048];
                    glGetProgramInfoLog(p.program, sizeof(log), nullptr, log);
                    HX_LOG_ERROR(GLLog, "pipeline {} link failed: {}", desc.debugName, StringView(log));
                    glDeleteProgram(p.program);
                    return {};
                }
                glUseProgram(p.program);
                currentProgram = p.program;
                ApplyBindings(p.program, *vs);
                ApplyBindings(p.program, *fs);
                appliedPipeline = {};
                return pipelines.Add(Move(p));
            }

            PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override
            {
                Shader* cs = shaders.Get(desc.computeShader);
                if (!cs || cs->stage != ShaderStage::Compute || !caps.compute)
                    return {};
                Pipeline p;
                p.compute = true;
                p.program = glCreateProgram();
                glAttachShader(p.program, cs->id);
                glLinkProgram(p.program);
                glDetachShader(p.program, cs->id);
                GLint ok = 0;
                glGetProgramiv(p.program, GL_LINK_STATUS, &ok);
                if (!ok)
                {
                    char log[2048];
                    glGetProgramInfoLog(p.program, sizeof(log), nullptr, log);
                    HX_LOG_ERROR(GLLog, "compute pipeline {} link failed: {}", desc.debugName, StringView(log));
                    glDeleteProgram(p.program);
                    return {};
                }
                glUseProgram(p.program);
                currentProgram = p.program;
                ApplyBindings(p.program, *cs);
                return pipelines.Add(Move(p));
            }

            void ApplyBindings(GLuint program, const Shader& s)
            {
                for (const BindingName& b : s.bindings)
                {
                    if (b.kind == BindingKind::UniformBuffer)
                    {
                        GLuint index = glGetUniformBlockIndex(program, b.name.CStr());
                        if (index != GL_INVALID_INDEX)
                            glUniformBlockBinding(program, index, b.slot);
                    }
                    else if (b.kind == BindingKind::Texture || (b.kind == BindingKind::StorageTexture && caps.backend == Backend::OpenGL))
                    {
                        GLint location = glGetUniformLocation(program, b.name.CStr());
                        if (location >= 0)
                            glUniform1i(location, GLint(b.slot));
                    }
                    else if (b.kind == BindingKind::StorageBuffer && api.ShaderStorageBlockBinding)
                    {
                        GLuint index = glGetProgramResourceIndex(program, GL_SHADER_STORAGE_BLOCK, b.name.CStr());
                        if (index != GL_INVALID_INDEX)
                            glShaderStorageBlockBinding(program, index, b.slot);
                    }
                }
            }

            void Destroy(BufferHandle h) override
            {
                Buffer b;
                if (!buffers.Remove(h, b))
                    return;
                if (b.inHeap)
                    heapRanges.Free(b.heapOffset, b.desc.size, heapAlignment);
                else
                    glDeleteBuffers(1, &b.id);
            }

            void Destroy(TextureHandle h) override
            {
                Texture t;
                if (!textures.Remove(h, t))
                    return;
                if (t.id)
                    glDeleteTextures(1, &t.id);
                if (t.renderbuffer)
                    glDeleteRenderbuffers(1, &t.renderbuffer);
                Array<u64> stale;
                for (auto e : framebuffers)
                    for (u32 texture : e.value.textures)
                        if (texture == h.value)
                        {
                            stale.Push(e.key);
                            break;
                        }
                for (u64 k : stale)
                {
                    glDeleteFramebuffers(1, &framebuffers.Find(k)->id);
                    framebuffers.Remove(k);
                }
            }

            void Destroy(SamplerHandle h) override
            {
                Sampler s;
                if (samplers.Remove(h, s))
                    glDeleteSamplers(1, &s.id);
            }

            void Destroy(ShaderHandle h) override
            {
                Shader s;
                if (shaders.Remove(h, s))
                    glDeleteShader(s.id);
            }

            void Destroy(PipelineHandle h) override
            {
                Pipeline p;
                if (pipelines.Remove(h, p))
                    glDeleteProgram(p.program);
                appliedPipeline = {};
            }

            u32 RegisterBindless(TextureHandle texture, SamplerHandle sampler) override
            {
                Texture* t = textures.Get(texture);
                Sampler* s = samplers.Get(sampler);
                if (!caps.bindlessTextures || !t || !s)
                    return kInvalidBindlessIndex;
                GLuint64 handle = glGetTextureSamplerHandleARB(t->id, s->id);
                if (!handle)
                    return kInvalidBindlessIndex;
                glMakeTextureHandleResidentARB(handle);
                return bindless.Add(handle);
            }

            void UnregisterBindless(u32 index) override
            {
                GLuint64 handle = 0;
                if (bindless.Remove(index, handle) && handle)
                    glMakeTextureHandleNonResidentARB(handle);
            }

            void UploadBindless()
            {
                if (!caps.bindlessTextures || bindless.Size() == 0)
                    return;
                if (!bindlessBuffer)
                    glGenBuffers(1, &bindlessBuffer);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, bindlessBuffer);
                if (bindless.Dirty())
                {
                    u32 bytes = bindless.Size() * u32(sizeof(GLuint64));
                    if (bytes > bindlessCapacity)
                    {
                        bindlessCapacity = Max(bytes * 2, 4096u);
                        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(bindlessCapacity), nullptr, GL_DYNAMIC_DRAW);
                    }
                    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(bytes), bindless.Data());
                    bindless.ClearDirty();
                }
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindlessGLTableBinding, bindlessBuffer);
            }

            TransientAllocation AllocateTransient(u32 size) override
            {
                TransientAllocation a{};
                if (size > transientSize)
                    return a;
                for (u32 spins = 0; !transientRing.TryAllocate(size, transient, a); spins++)
                {
                    // Only the device thread may talk to GL; other threads wait for it to release memory.
                    if (platform::CurrentThreadId() == deviceThread)
                        ReleaseTransient(true);
                    else if (spins < 200000)
                        platform::YieldThread();
                    else
                        OverrunTransient();
                }
                return a;
            }

            void UpdateTransientLimit()
            {
                transientRing.SetLimit((transientGroups.Empty() ? transientFenced : transientGroups[0].begin) + transientSize);
            }

            // The ring is too small for the frames in flight: give up protecting the oldest data instead of hanging.
            void OverrunTransient()
            {
                if (!transientWarned)
                    HX_LOG_ERROR(GLLog, "transient ring of {} bytes is too small for one frame, uniform data may be overwritten", transientSize);
                transientWarned = true;
                transientRing.SetLimit(transientRing.Head() + transientSize);
            }

            // Frees the groups whose fences have passed; with `block` it waits for the oldest one when nothing is free yet.
            void ReleaseTransient(bool block)
            {
                bool released = false;
                while (!transientGroups.Empty() && !transientGroups[0].open)
                {
                    GLenum state = glClientWaitSync(transientGroups[0].sync, 0, 0);
                    if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED)
                    {
                        if (!block || released)
                            break;
                        while (glClientWaitSync(transientGroups[0].sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) == GL_TIMEOUT_EXPIRED)
                        {
                        }
                    }
                    glDeleteSync(transientGroups[0].sync);
                    transientGroups.RemoveAt(0);
                    released = true;
                }
                if (released)
                    UpdateTransientLimit();
                else if (block)
                    OverrunTransient();
            }

            // Also ends the thread chunks outside frames: a chunk kept longer could hand out old memory right before the
            // ring wraps, and the next lap would give the same bytes to another thread.
            void FenceTransient()
            {
                if (!transientMapped)
                {
                    if (!inFrame)
                        transientRing.NextEpoch();
                    return;
                }
                u64 head = transientRing.Head();
                if (inFrame && !transientGroups.Empty() && transientGroups.Back().open)
                {
                    TransientGroup& g = transientGroups.Back();
                    glDeleteSync(g.sync);
                    g.sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    g.end = transientFenced = head;
                    return;
                }
                if (head == transientFenced)
                    return;
                transientGroups.Push({glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), transientFenced, head, inFrame});
                transientFenced = head;
                if (!inFrame)
                    transientRing.NextEpoch();
            }

            // Without a persistent mapping the ranges this list reads are copied into the buffer; a wrapped ring gets a
            // fresh buffer store first, because the GPU may still read the old contents.
            void UploadTransient(const CommandList& commands)
            {
                Span<const CommandList::TransientRange> ranges = commands.TransientRanges();
                if (transientMapped || ranges.Empty())
                    return;
                glBindBuffer(GL_UNIFORM_BUFFER, transientId);
                bool orphan = false;
                for (const CommandList::TransientRange& r : ranges)
                    orphan = orphan || transientUploads.Conflicts(r.begin, Min(r.end, transientSize), r.generation);
                if (orphan)
                {
                    transientUploads.Clear();
                    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(transientSize), nullptr, GL_STREAM_DRAW);
                }
                for (const CommandList::TransientRange& r : ranges)
                {
                    transientUploads.Add(r.begin, Min(r.end, transientSize), r.generation, [&](u32 begin, u32 end) {
                        glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(begin), GLsizeiptr(end - begin), transientCpu.Data() + begin);
                    });
                }
            }

            void BeginFrame() override
            {
                context->MakeCurrent();
                deviceThread = platform::CurrentThreadId();
                inFrame = true;
                // Thread chunks end with the frame, see FenceTransient.
                transientRing.NextEpoch();
                if (!transientMapped)
                    return;
                // Start the frame with at least half of the ring free, so worker threads rarely have to wait for the GPU.
                ReleaseTransient(false);
                while (transientRing.Limit() - transientRing.Head() < transientSize / 2 && !transientGroups.Empty() && !transientGroups[0].open)
                    ReleaseTransient(true);
            }

            void EndFrame() override
            {
                timers.Close();
                PollTimers();
                timers.Advance();
                inFrame = false;
                if (transientMapped)
                {
                    if (!transientGroups.Empty() && transientGroups.Back().open)
                    {
                        transientGroups.Back().open = false;
                        transientGroups.Back().end = transientFenced = transientRing.Head();
                    }
                }
                transientRing.NextEpoch();
            }

            void PollTimers()
            {
                if (!caps.gpuTimers)
                    return;
                timers.ForEachPending([&](u32 index, GpuTimerFrames::Frame& f) {
                    Array<GLuint>& queries = timestampQueries[index];
                    GLuint available = 0;
                    if (timerExt)
                        glGetQueryObjectuivEXT(queries[f.queries - 1], GL_QUERY_RESULT_AVAILABLE, &available);
                    else
                        glGetQueryObjectuiv(queries[f.queries - 1], GL_QUERY_RESULT_AVAILABLE, &available);
                    if (!available)
                        return false;
                    Array<u64> ticks;
                    ticks.Resize(f.queries);
                    for (u32 q = 0; q < f.queries; q++)
                    {
                        if (timerExt)
                            glGetQueryObjectui64vEXT(queries[q], GL_QUERY_RESULT, &ticks[q]);
                        else
                            glGetQueryObjectui64v(queries[q], GL_QUERY_RESULT, &ticks[q]);
                    }
                    GLint disjoint = 0;
                    if (timerExt)
                        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
                    timers.Resolve(f, ticks.Data(), 1000000000ull, !disjoint);
                    return true;
                });
            }

            u32 ReadGpuTimings(GpuTiming* out, u32 max) override
            {
                PollTimers();
                return timers.Read(out, max);
            }

            void WriteTimestamp(i32 index)
            {
                if (index < 0 || !caps.gpuTimers)
                    return;
                Array<GLuint>& queries = timestampQueries[timers.CurrentIndex()];
                while (queries.Size() <= usize(index))
                {
                    GLuint q = 0;
                    glGenQueries(1, &q);
                    queries.Push(q);
                }
                if (timerExt)
                {
                    if (index == 0)
                    {
                        GLint disjoint = 0;
                        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
                    }
                    glQueryCounterEXT(queries[usize(index)], GL_TIMESTAMP_EXT);
                }
                else
                    glQueryCounter(queries[usize(index)], GL_TIMESTAMP);
            }

            // Writes from compute become visible to every later use; issued lazily so back to back dispatches share one barrier.
            void Barrier()
            {
                if (!barrierPending)
                    return;
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                barrierPending = false;
            }

            void UseProgram(GLuint program)
            {
                if (currentProgram != program)
                {
                    glUseProgram(program);
                    currentProgram = program;
                }
            }

            bool PrepareDraw(u32 firstInstance)
            {
                Barrier();
                if (!pipeline)
                    return false;
                UseProgram(pipeline->program);
                if (attributesDirty || (firstInstance && !caps.baseInstance))
                    SetupAttributes(firstInstance);
                return true;
            }
            void Present() override { context->Swap(); }
            void Resize(u32, u32) override {}
            void WaitIdle() override { glFinish(); }

            // A texture's mip and layer (a cube's face) at an attachment point of the bound framebuffer.
            void Attach(GLenum framebuffer, GLenum point, const Texture& t, u32 mip, u32 layer)
            {
                if (t.renderbuffer)
                    glFramebufferRenderbuffer(framebuffer, point, GL_RENDERBUFFER, t.renderbuffer);
                else if (t.target == GL_TEXTURE_CUBE_MAP)
                    glFramebufferTexture2D(framebuffer, point, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, t.id, GLint(mip));
                else if (t.target == GL_TEXTURE_2D_ARRAY || t.target == GL_TEXTURE_3D)
                    glFramebufferTextureLayer(framebuffer, point, t.id, GLint(mip), GLint(layer));
                else
                    glFramebufferTexture2D(framebuffer, point, GL_TEXTURE_2D, t.id, GLint(mip));
            }

            static GLenum DepthPoint(Format f) { return f == Format::D24UnormS8Uint ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT; }

            GLuint FramebufferFor(const RenderPassDesc& rp)
            {
                u64 key = 1469598103934665603ull;
                auto mix = [&](u64 v) { key = (key ^ v) * 1099511628211ull; };
                for (u32 i = 0; i < rp.colorCount; i++)
                    mix(u64(rp.colors[i].texture.value) | u64(rp.colors[i].mipLevel) << 32 | u64(rp.colors[i].layer) << 40);
                mix(u64(rp.depth.texture.value) | u64(rp.depth.mipLevel) << 32 | u64(rp.depth.layer) << 40 | 1ull << 63);
                if (Framebuffer* existing = framebuffers.Find(key))
                    return existing->id;
                Framebuffer fb;
                glGenFramebuffers(1, &fb.id);
                glBindFramebuffer(GL_FRAMEBUFFER, fb.id);
                GLenum drawBuffers[kMaxColorTargets];
                for (u32 i = 0; i < rp.colorCount; i++)
                {
                    Texture* t = textures.Get(rp.colors[i].texture);
                    drawBuffers[i] = t ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
                    if (t)
                    {
                        Attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, *t, rp.colors[i].mipLevel, rp.colors[i].layer);
                        fb.textures[i] = rp.colors[i].texture.value;
                    }
                }
                glDrawBuffers(GLsizei(rp.colorCount), drawBuffers);
                if (Texture* d = textures.Get(rp.depth.texture))
                {
                    Attach(GL_FRAMEBUFFER, DepthPoint(d->desc.format), *d, rp.depth.mipLevel, rp.depth.layer);
                    fb.textures[kMaxColorTargets] = rp.depth.texture.value;
                }
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    HX_LOG_ERROR(GLLog, "incomplete framebuffer");
                framebuffers.Set(key, fb);
                return fb.id;
            }

            // One subresource into another through the two blit framebuffers (resolves multisampled ones too).
            void Blit(TextureHandle from, u32 fromMip, u32 fromLayer, TextureHandle to, u32 toMip, u32 toLayer)
            {
                Texture* s = textures.Get(from);
                Texture* d = textures.Get(to);
                if (!s || !d)
                    return;
                bool depth = IsDepthFormat(s->desc.format);
                GLenum point = depth ? DepthPoint(s->desc.format) : GL_COLOR_ATTACHMENT0;
                Barrier();
                glDisable(GL_SCISSOR_TEST);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
                Attach(GL_READ_FRAMEBUFFER, point, *s, fromMip, fromLayer);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blitFramebuffer);
                Attach(GL_DRAW_FRAMEBUFFER, point, *d, toMip, toLayer);
                if (!depth)
                {
                    glReadBuffer(GL_COLOR_ATTACHMENT0);
                    GLenum buffer = GL_COLOR_ATTACHMENT0;
                    glDrawBuffers(1, &buffer);
                }
                GLint sw = GLint(Max(1u, s->desc.width >> fromMip)), sh = GLint(Max(1u, s->desc.height >> fromMip));
                GLint dw = GLint(Max(1u, d->desc.width >> toMip)), dh = GLint(Max(1u, d->desc.height >> toMip));
                GLbitfield mask = depth ? (s->desc.format == Format::D24UnormS8Uint ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_DEPTH_BUFFER_BIT)
                                        : GL_COLOR_BUFFER_BIT;
                glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, mask, GL_NEAREST);
                // Detached again: the blit framebuffers hold nothing between commands.
                glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, point, GL_RENDERBUFFER, 0);
                glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, point, GL_RENDERBUFFER, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                appliedPipeline = {};
            }

            void ApplyPipeline(Pipeline* p)
            {
                const PipelineDesc& d = p->desc;
                UseProgram(p->program);
                if (d.cull == CullMode::None)
                    glDisable(GL_CULL_FACE);
                else
                {
                    glEnable(GL_CULL_FACE);
                    glCullFace(d.cull == CullMode::Back ? GL_BACK : GL_FRONT);
                }
                frontCounterClockwise = d.frontCounterClockwise;
                ApplyFrontFace();
                if (d.depthTest || d.depthWrite)
                {
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(d.depthTest ? ToCompare(d.depthCompare) : GL_ALWAYS);
                }
                else
                    glDisable(GL_DEPTH_TEST);
                glDepthMask(d.depthWrite ? GL_TRUE : GL_FALSE);
                const BlendState& b = d.blend[0];
                if (b.enabled)
                {
                    glEnable(GL_BLEND);
                    glBlendFuncSeparate(ToBlend(b.srcColor), ToBlend(b.dstColor), ToBlend(b.srcAlpha), ToBlend(b.dstAlpha));
                    glBlendEquationSeparate(ToBlendOp(b.colorOp), ToBlendOp(b.alphaOp));
                }
                else
                    glDisable(GL_BLEND);
                glColorMask((b.writeMask & 1) != 0, (b.writeMask & 2) != 0, (b.writeMask & 4) != 0, (b.writeMask & 8) != 0);
                // OpenGL ES has no polygon mode, though some loaders still find the desktop entry point.
                if (api.PolygonMode && caps.backend == Backend::OpenGL)
                    glPolygonMode(GL_FRONT_AND_BACK, d.wireframe ? GL_LINE : GL_FILL);
                attributesDirty = true;
            }

            // An upper left clip origin also turns the facing test around, so the winding stays as in Direct3D.
            void ApplyFrontFace() { glFrontFace(frontCounterClockwise ? GL_CCW : GL_CW); }

            void SetupAttributes(u32 firstInstance)
            {
                if (!pipeline)
                    return;
                const PipelineDesc& d = pipeline->desc;
                u32 used = 0;
                for (u32 i = 0; i < d.attributeCount; i++)
                {
                    const VertexAttribute& a = d.attributes[i];
                    const VertexBufferLayout& layout = d.vertexBuffers[a.bufferSlot];
                    const VertexBinding& vb = vertexBuffers[a.bufferSlot];
                    VertexFormatInfo f = ToVertex(a.format);
                    u32 offset = vb.offset + a.offset;
                    if (layout.step == StepMode::Instance && !caps.baseInstance)
                        offset += firstInstance * layout.stride;
                    glBindBuffer(GL_ARRAY_BUFFER, vb.buffer);
                    glEnableVertexAttribArray(a.location);
                    const void* pointer = reinterpret_cast<const void*>(uptr(offset));
                    if (f.integer)
                        glVertexAttribIPointer(a.location, f.components, f.type, GLsizei(layout.stride), pointer);
                    else
                        glVertexAttribPointer(a.location, f.components, f.type, f.normalized, GLsizei(layout.stride), pointer);
                    glVertexAttribDivisor(a.location, layout.step == StepMode::Instance ? 1 : 0);
                    used |= 1u << a.location;
                }
                for (u32 l = 0; l < 32; l++)
                    if ((enabledAttributes & ~used) & (1u << l))
                        glDisableVertexAttribArray(l);
                enabledAttributes = used;
                attributesDirty = false;
            }

            void SetRect(bool scissor, i32 x, i32 y, i32 w, i32 h)
            {
                i32 gy = offscreen && caps.clipControl ? y : i32(targetHeight) - (y + h);
                if (scissor)
                    glScissor(x, gy, w, h);
                else
                    glViewport(x, gy, w, h);
            }

            void Submit(const CommandList& commands) override
            {
                UploadTransient(commands);
                UploadBindless();
                UploadBufferTable();
                glBindVertexArray(vao);
                commands.ForEach([&](CommandType type, const u8* data) {
                    switch (type)
                    {
                    case CommandType::BeginRenderPass:
                    {
                        const RenderPassDesc& rp = CommandData<CmdBeginRenderPass>(data).desc;
                        Barrier();
                        Texture* first = textures.Get(rp.colors[0].texture);
                        Texture* depth = textures.Get(rp.depth.texture);
                        offscreen = first || depth;
                        u32 width = 0, height = 0;
                        if (offscreen)
                        {
                            glBindFramebuffer(GL_FRAMEBUFFER, FramebufferFor(rp));
                            Texture* sizeSource = first ? first : depth;
                            u32 mip = first ? rp.colors[0].mipLevel : rp.depth.mipLevel;
                            width = Max(1u, sizeSource->desc.width >> mip);
                            height = Max(1u, sizeSource->desc.height >> mip);
                        }
                        else
                        {
                            glBindFramebuffer(GL_FRAMEBUFFER, 0);
                            context->GetDrawableSize(width, height);
                        }
                        targetHeight = height;
                        if (caps.clipControl)
                            glClipControl(offscreen ? GL_UPPER_LEFT : GL_LOWER_LEFT, GL_ZERO_TO_ONE);
                        ApplyFrontFace();
                        glDisable(GL_SCISSOR_TEST);
                        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glDepthMask(GL_TRUE);
                        appliedPipeline = {};
                        for (u32 i = 0; i < rp.colorCount; i++)
                            if (rp.colors[i].load == LoadOp::Clear)
                                glClearBufferfv(GL_COLOR, GLint(i), rp.colors[i].clearColor);
                        if ((depth || !offscreen) && rp.depth.load == LoadOp::Clear)
                            glClearBufferfi(GL_DEPTH_STENCIL, 0, rp.depth.clearDepth, rp.depth.clearStencil);
                        glEnable(GL_SCISSOR_TEST);
                        SetRect(false, 0, 0, i32(width), i32(height));
                        SetRect(true, 0, 0, i32(width), i32(height));
                        break;
                    }
                    case CommandType::EndRenderPass:
                        break;
                    case CommandType::SetViewport:
                    {
                        const Viewport& v = CommandData<CmdSetViewport>(data).viewport;
                        SetRect(false, i32(v.x), i32(v.y), i32(v.width), i32(v.height));
                        glDepthRangef(v.minDepth, v.maxDepth);
                        break;
                    }
                    case CommandType::SetScissor:
                    {
                        const ScissorRect& r = CommandData<CmdSetScissor>(data).rect;
                        SetRect(true, r.x, r.y, i32(r.width), i32(r.height));
                        break;
                    }
                    case CommandType::SetPipeline:
                    {
                        PipelineHandle h = CommandData<CmdSetPipeline>(data).pipeline;
                        Pipeline* p = pipelines.Get(h);
                        if (p && p->compute)
                        {
                            computePipeline = p;
                            break;
                        }
                        pipeline = p;
                        if (pipeline && !(h == appliedPipeline))
                        {
                            ApplyPipeline(pipeline);
                            appliedPipeline = h;
                        }
                        break;
                    }
                    case CommandType::SetVertexBuffer:
                    {
                        const CmdSetVertexBuffer& c = CommandData<CmdSetVertexBuffer>(data);
                        Buffer* b = buffers.Get(c.buffer);
                        VertexBinding next{b ? b->id : 0, c.offset + (b ? b->heapOffset : 0)};
                        if (vertexBuffers[c.slot].buffer != next.buffer || vertexBuffers[c.slot].offset != next.offset)
                        {
                            vertexBuffers[c.slot] = next;
                            attributesDirty = true;
                        }
                        break;
                    }
                    case CommandType::SetIndexBuffer:
                    {
                        const CmdSetIndexBuffer& c = CommandData<CmdSetIndexBuffer>(data);
                        Buffer* b = buffers.Get(c.buffer);
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b ? b->id : 0);
                        indexType = c.type == IndexType::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
                        indexOffset = c.offset + (b ? b->heapOffset : 0);
                        break;
                    }
                    case CommandType::SetUniformBuffer:
                    {
                        const CmdSetUniformBuffer& c = CommandData<CmdSetUniformBuffer>(data);
                        if (Buffer* b = buffers.Get(c.buffer))
                            glBindBufferRange(GL_UNIFORM_BUFFER, c.slot, b->id, GLintptr(c.offset), GLsizeiptr(c.size));
                        break;
                    }
                    case CommandType::SetTexture:
                    {
                        const CmdSetTexture& c = CommandData<CmdSetTexture>(data);
                        Texture* t = textures.Get(c.texture);
                        Sampler* s = samplers.Get(c.sampler);
                        glActiveTexture(GL_TEXTURE0 + c.slot);
                        glBindTexture(t ? t->target : GL_TEXTURE_2D, t ? t->id : 0);
                        glBindSampler(c.slot, s ? s->id : 0);
                        break;
                    }
                    case CommandType::Draw:
                    {
                        const CmdDraw& c = CommandData<CmdDraw>(data);
                        PrepareDraw(c.firstInstance);
                        GLenum mode = ToTopology(pipeline ? pipeline->desc.topology : PrimitiveTopology::TriangleList);
                        if (c.firstInstance && caps.baseInstance)
                            glDrawArraysInstancedBaseInstance(mode, GLint(c.firstVertex), GLsizei(c.vertexCount), GLsizei(c.instanceCount), c.firstInstance);
                        else if (c.instanceCount != 1)
                            glDrawArraysInstanced(mode, GLint(c.firstVertex), GLsizei(c.vertexCount), GLsizei(c.instanceCount));
                        else
                            glDrawArrays(mode, GLint(c.firstVertex), GLsizei(c.vertexCount));
                        if (c.firstInstance && !caps.baseInstance)
                            attributesDirty = true;
                        break;
                    }
                    case CommandType::DrawIndexed:
                    {
                        const CmdDrawIndexed& c = CommandData<CmdDrawIndexed>(data);
                        PrepareDraw(c.firstInstance);
                        GLenum mode = ToTopology(pipeline ? pipeline->desc.topology : PrimitiveTopology::TriangleList);
                        u32 indexSize = indexType == GL_UNSIGNED_SHORT ? 2 : 4;
                        const void* offset = reinterpret_cast<const void*>(uptr(indexOffset + c.firstIndex * indexSize));
                        if (caps.baseInstance)
                            glDrawElementsInstancedBaseVertexBaseInstance(mode, GLsizei(c.indexCount), indexType, offset, GLsizei(c.instanceCount), c.baseVertex,
                                                                          c.firstInstance);
                        else if (c.baseVertex && api.DrawElementsInstancedBaseVertex)
                            glDrawElementsInstancedBaseVertex(mode, GLsizei(c.indexCount), indexType, offset, GLsizei(c.instanceCount), c.baseVertex);
                        else if (c.instanceCount != 1)
                            glDrawElementsInstanced(mode, GLsizei(c.indexCount), indexType, offset, GLsizei(c.instanceCount));
                        else
                            glDrawElements(mode, GLsizei(c.indexCount), indexType, offset);
                        if (c.firstInstance && !caps.baseInstance)
                            attributesDirty = true;
                        break;
                    }
                    case CommandType::UpdateBuffer:
                    {
                        const CmdUpdateBuffer& c = CommandData<CmdUpdateBuffer>(data);
                        Barrier();
                        if (Buffer* b = buffers.Get(c.buffer))
                        {
                            glBindBuffer(GL_COPY_WRITE_BUFFER, b->id);
                            glBufferSubData(GL_COPY_WRITE_BUFFER, GLintptr(c.offset + b->heapOffset), GLsizeiptr(c.size), data + sizeof(CmdUpdateBuffer));
                        }
                        break;
                    }
                    case CommandType::CopyBuffer:
                    {
                        const CmdCopyBuffer& c = CommandData<CmdCopyBuffer>(data);
                        Barrier();
                        Buffer* src = buffers.Get(c.source);
                        Buffer* dst = buffers.Get(c.destination);
                        if (src && dst)
                        {
                            glBindBuffer(GL_COPY_READ_BUFFER, src->id);
                            glBindBuffer(GL_COPY_WRITE_BUFFER, dst->id);
                            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, GLintptr(c.sourceOffset + src->heapOffset),
                                                GLintptr(c.destinationOffset + dst->heapOffset), GLsizeiptr(c.size));
                        }
                        break;
                    }
                    case CommandType::CopyTextureToBuffer:
                        break;
                    case CommandType::SetStorageBuffer:
                    {
                        const CmdSetStorageBuffer& c = CommandData<CmdSetStorageBuffer>(data);
                        Buffer* b = buffers.Get(c.buffer);
                        if (!caps.storageBuffers || c.slot >= kMaxStorageSlots)
                            break;
                        if (b && b->inHeap)
                            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, c.slot, b->id, GLintptr(b->heapOffset), GLsizeiptr(b->desc.size));
                        else
                            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, c.slot, b ? b->id : 0);
                        break;
                    }
                    case CommandType::SetStorageTexture:
                    {
                        const CmdSetStorageTexture& c = CommandData<CmdSetStorageTexture>(data);
                        Texture* t = textures.Get(c.texture);
                        if (!caps.compute || c.slot >= kMaxStorageSlots)
                            break;
                        if (t)
                            glBindImageTexture(c.slot, t->id, GLint(c.mipLevel), t->target != GL_TEXTURE_2D, 0, GL_READ_WRITE, ToGL(t->desc.format).internal);
                        else
                            glBindImageTexture(c.slot, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
                        break;
                    }
                    case CommandType::Dispatch:
                    case CommandType::DispatchIndirect:
                    {
                        if (!computePipeline)
                            break;
                        Barrier();
                        UseProgram(computePipeline->program);
                        if (type == CommandType::Dispatch)
                        {
                            const CmdDispatch& c = CommandData<CmdDispatch>(data);
                            glDispatchCompute(c.x, c.y, c.z);
                        }
                        else
                        {
                            const CmdIndirect& c = CommandData<CmdIndirect>(data);
                            Buffer* b = buffers.Get(c.buffer);
                            if (!b)
                                break;
                            glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, b->id);
                            glDispatchComputeIndirect(GLintptr(c.offset + b->heapOffset));
                        }
                        barrierPending = true;
                        break;
                    }
                    case CommandType::DrawIndirect:
                    case CommandType::DrawIndexedIndirect:
                    {
                        const CmdIndirect& c = CommandData<CmdIndirect>(data);
                        Buffer* b = buffers.Get(c.buffer);
                        if (!b || !caps.drawIndirect || !PrepareDraw(0))
                            break;
                        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, b->id);
                        GLenum mode = ToTopology(pipeline->desc.topology);
                        bool indexed = type == CommandType::DrawIndexedIndirect;
                        const void* offset = reinterpret_cast<const void*>(uptr(c.offset + b->heapOffset));
                        if (caps.multiDrawIndirect && c.drawCount > 1)
                        {
                            if (indexed)
                                glMultiDrawElementsIndirect(mode, indexType, offset, GLsizei(c.drawCount), GLsizei(c.stride));
                            else
                                glMultiDrawArraysIndirect(mode, offset, GLsizei(c.drawCount), GLsizei(c.stride));
                            break;
                        }
                        for (u32 i = 0; i < c.drawCount; i++)
                        {
                            const void* at = reinterpret_cast<const void*>(uptr(b->heapOffset + c.offset + i * c.stride));
                            if (indexed)
                                glDrawElementsIndirect(mode, indexType, at);
                            else
                                glDrawArraysIndirect(mode, at);
                        }
                        break;
                    }
                    case CommandType::BeginGpuScope:
                    {
                        const char* name;
                        std::memcpy(&name, data, sizeof(name));
                        WriteTimestamp(timers.Begin(name));
                        break;
                    }
                    case CommandType::EndGpuScope:
                        WriteTimestamp(timers.End());
                        break;
                    case CommandType::ResolveTexture:
                    {
                        const CmdResolveTexture& c = CommandData<CmdResolveTexture>(data);
                        Blit(c.source, 0, 0, c.destination, 0, 0);
                        break;
                    }
                    case CommandType::CopyTexture:
                    {
                        const CmdCopyTexture& c = CommandData<CmdCopyTexture>(data);
                        Blit(c.source, c.sourceMip, c.sourceLayer, c.destination, c.destinationMip, c.destinationLayer);
                        break;
                    }
                    case CommandType::GenerateMips:
                    {
                        if (Texture* t = textures.Get(CommandData<CmdGenerateMips>(data).texture); t && t->id)
                        {
                            Barrier();
                            glBindTexture(t->target, t->id);
                            glGenerateMipmap(t->target);
                        }
                        break;
                    }
                    }
                });
                FenceTransient();
            }

            bool ReadTexture(TextureHandle handle, u32 mip, void* out, usize size) override
            {
                Texture* t = textures.Get(handle);
                if (!t || t->target != GL_TEXTURE_2D || IsDepthFormat(t->desc.format))
                    return false;
                Barrier();
                u32 w = Max(1u, t->desc.width >> mip), h = Max(1u, t->desc.height >> mip), bpp = FormatBytesPerPixel(t->desc.format);
                usize row = usize(w) * bpp;
                if (size < row * h)
                    return false;
                GLFormat f = ToGL(t->desc.format);
                glBindFramebuffer(GL_FRAMEBUFFER, readFramebuffer);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, GLint(mip));
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                // OpenGL ES reads one and two channel formats only as four channels: read those and keep the first ones.
                u32 channels = f.format == GL_RED || f.format == GL_RED_INTEGER ? 1u : f.format == GL_RG || f.format == GL_RG_INTEGER ? 2u : 0u;
                bool widen = caps.backend == Backend::OpenGLES && channels != 0 &&
                             (f.type == GL_FLOAT || f.type == GL_UNSIGNED_INT || f.type == GL_INT || f.type == GL_UNSIGNED_BYTE);
                if (widen)
                {
                    u32 component = bpp / channels;
                    Array<u8> four;
                    four.Resize(usize(w) * h * 4 * component);
                    bool integer = f.format == GL_RED_INTEGER || f.format == GL_RG_INTEGER;
                    glReadPixels(0, 0, GLsizei(w), GLsizei(h), integer ? GL_RGBA_INTEGER : GL_RGBA, f.type, four.Data());
                    u8* bytes = static_cast<u8*>(out);
                    for (usize p = 0; p < usize(w) * h; p++)
                        std::memcpy(bytes + p * bpp, four.Data() + p * 4 * component, bpp);
                }
                else if (caps.backend == Backend::OpenGLES && f.type == GL_HALF_FLOAT)
                {
                    // Half floats are read as floats there, then rounded back.
                    u32 count = bpp / 2;
                    Array<f32> floats;
                    floats.Resize(usize(w) * h * 4);
                    glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_FLOAT, floats.Data());
                    u16* halves = static_cast<u16*>(out);
                    for (usize p = 0; p < usize(w) * h; p++)
                        for (u32 c = 0; c < count; c++)
                            halves[p * count + c] = FloatToHalf(floats[p * 4 + c]);
                }
                else
                    glReadPixels(0, 0, GLsizei(w), GLsizei(h), f.format, f.type, out);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (!caps.clipControl)
                {
                    Array<u8> tmp;
                    tmp.Resize(row);
                    u8* bytes = static_cast<u8*>(out);
                    for (u32 y = 0; y < h / 2; y++)
                    {
                        std::memcpy(tmp.Data(), bytes + y * row, row);
                        std::memcpy(bytes + y * row, bytes + (h - 1 - y) * row, row);
                        std::memcpy(bytes + (h - 1 - y) * row, tmp.Data(), row);
                    }
                }
                appliedPipeline = {};
                return glGetError() == GL_NO_ERROR;
            }

            bool ReadBuffer(BufferHandle handle, u32 offset, void* out, u32 size) override
            {
                Buffer* b = buffers.Get(handle);
                if (!b)
                    return false;
                Barrier();
                glBindBuffer(GL_COPY_READ_BUFFER, b->id);
                offset += b->heapOffset;
                // OpenGL ES has no glGetBufferSubData, even where the driver of a desktop exports it.
                if (api.GetBufferSubData && caps.backend != Backend::OpenGLES)
                {
                    glGetBufferSubData(GL_COPY_READ_BUFFER, GLintptr(offset), GLsizeiptr(size), out);
                    return true;
                }
                if (!api.MapBufferRange)
                    return false;
                void* p = glMapBufferRange(GL_COPY_READ_BUFFER, GLintptr(offset), GLsizeiptr(size), GL_MAP_READ_BIT);
                if (!p)
                    return false;
                std::memcpy(out, p, size);
                glUnmapBuffer(GL_COPY_READ_BUFFER);
                return true;
            }

            void DestroySelf() override
            {
                Shutdown();
                Delete(HeapAllocator(), this);
            }
        };
    }

    bool GLAvailable(Backend backend)
    {
#if HX_PLATFORM_WEB
        return backend == Backend::WebGL2;
#elif HX_PLATFORM_ANDROID
        return backend == Backend::OpenGLES;
#else
        return backend != Backend::WebGL2 && gl::OpenSystemLibraries();
#endif
    }

    Device* CreateGLDevice(const DeviceDesc& desc)
    {
        if (!GLAvailable(desc.backend))
            return nullptr;
        auto* d = New<GLDevice>(HeapAllocator());
        if (!d->Init(desc))
        {
            d->Shutdown();
            Delete(HeapAllocator(), d);
            return nullptr;
        }
        return d;
    }
}
