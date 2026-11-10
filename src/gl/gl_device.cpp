#include "common/bindless_table.h"
#include "common/pool.h"
#include "gl/gl_context.h"

#include <helix/core/containers/hash_map.h>
#include <helix/core/log/log.h>
#include <helix/core/strings/string.h>
#include <helix/gl/loader.h>

#include <cstdio>
#include <cstring>

namespace helix::rhi
{
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
            case Format::RGB10A2Unorm: return {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, false};
            case Format::RG11B10Float: return {GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, false};
            case Format::D16Unorm: return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, false};
            case Format::D24UnormS8Uint: return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, false};
            case Format::D32Float: return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, false};
            case Format::BC1Unorm: return {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, true};
            case Format::BC3Unorm: return {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, true};
            case Format::BC5Unorm: return {GL_COMPRESSED_RG_RGTC2, 0, 0, true};
            case Format::BC7Unorm: return {GL_COMPRESSED_RGBA_BPTC_UNORM, 0, 0, true};
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
        };

        struct Texture
        {
            GLuint id = 0;
            GLenum target = GL_TEXTURE_2D;
            TextureDesc desc{};
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
            HashMap<u64, GLuint> framebuffers;
            GLuint vao = 0;
            GLuint readFramebuffer = 0;

            BufferHandle transient;
            GLuint transientId = 0;
            Array<u8> transientCpu;
            u32 transientSize = 0, transientHead = 0, transientUploaded = 0;
            bool transientOrphan = true;

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
                caps.apiMajor = u32(major);
                caps.apiMinor = u32(minor);
                bool web = caps.backend == Backend::WebGL2;
                caps.compute = !web && (es ? at(3, 1) : at(4, 3));
                caps.storageBuffers = caps.compute;
                caps.multiDrawIndirect = !es && (at(4, 3) || HasExtension("GL_ARB_multi_draw_indirect"));
                caps.bindlessTextures = HasExtension("GL_ARB_bindless_texture") && at(4, 3) && api.GetTextureSamplerHandleARB &&
                                        api.MakeTextureHandleResidentARB;
                caps.maxBindlessTextures = caps.bindlessTextures ? 1u << 20 : 0;
                caps.baseInstance = !es && at(4, 2);
                caps.uniformBufferOffsets = true;
                caps.persistentMapping = !es && (at(4, 4) || HasExtension("GL_ARB_buffer_storage"));
                caps.clipControl = api.ClipControl != nullptr && (at(4, 5) || HasExtension("GL_ARB_clip_control") || HasExtension("GL_EXT_clip_control"));
                caps.textureCompressionBC = HasExtension("GL_EXT_texture_compression_s3tc") || HasExtension("WEBGL_compressed_texture_s3tc");
                caps.textureCompressionETC2 = es;
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
                if (!es)
                    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

                transientSize = desc.transientBufferSize;
                transientCpu.Resize(transientSize);
                glGenBuffers(1, &transientId);
                glBindBuffer(GL_UNIFORM_BUFFER, transientId);
                glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(transientSize), nullptr, GL_STREAM_DRAW);
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
                buffers.ForEach([](Buffer& b) { glDeleteBuffers(1, &b.id); });
                textures.ForEach([](Texture& t) { glDeleteTextures(1, &t.id); });
                samplers.ForEach([](Sampler& s) { glDeleteSamplers(1, &s.id); });
                shaders.ForEach([](Shader& s) { glDeleteShader(s.id); });
                pipelines.ForEach([](Pipeline& p) { glDeleteProgram(p.program); });
                for (auto e : framebuffers)
                    glDeleteFramebuffers(1, &e.value);
                for (u32 i = 0; i < bindless.Size(); i++)
                    if (bindless.Data()[i])
                        glMakeTextureHandleNonResidentARB(bindless.Data()[i]);
                if (bindlessBuffer)
                    glDeleteBuffers(1, &bindlessBuffer);
                glDeleteFramebuffers(1, &readFramebuffer);
                glDeleteVertexArrays(1, &vao);
                DestroyGLContext(context);
                context = nullptr;
            }

            const DeviceCaps& Caps() const override { return caps; }

            BufferHandle CreateBuffer(const BufferDesc& desc) override
            {
                Buffer b;
                b.desc = desc;
                b.desc.initialData = nullptr;
                glGenBuffers(1, &b.id);
                glBindBuffer(GL_COPY_WRITE_BUFFER, b.id);
                GLenum usage = desc.memory == MemoryType::Upload ? GL_DYNAMIC_DRAW : desc.memory == MemoryType::Readback ? GL_STREAM_READ : GL_STATIC_DRAW;
                glBufferData(GL_COPY_WRITE_BUFFER, GLsizeiptr(desc.size), desc.initialData, usage);
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
                glGenTextures(1, &t.id);
                glBindTexture(t.target, t.id);
                GLsizei mips = GLsizei(desc.mipLevels);
                if (t.target == GL_TEXTURE_2D || t.target == GL_TEXTURE_CUBE_MAP)
                    glTexStorage2D(t.target, mips, f.internal, GLsizei(desc.width), GLsizei(desc.height));
                else
                    glTexStorage3D(t.target, mips, f.internal, GLsizei(desc.width), GLsizei(desc.height), GLsizei(desc.depthOrLayers));
                glTexParameteri(t.target, GL_TEXTURE_MAX_LEVEL, mips - 1);
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
                for (Shader* s : {vs, fs})
                {
                    for (const BindingName& b : s->bindings)
                    {
                        if (b.kind == BindingKind::UniformBuffer)
                        {
                            GLuint index = glGetUniformBlockIndex(p.program, b.name.CStr());
                            if (index != GL_INVALID_INDEX)
                                glUniformBlockBinding(p.program, index, b.slot);
                        }
                        else if (b.kind == BindingKind::Texture)
                        {
                            GLint location = glGetUniformLocation(p.program, b.name.CStr());
                            if (location >= 0)
                                glUniform1i(location, GLint(b.slot));
                        }
                        else if (api.ShaderStorageBlockBinding)
                        {
                            GLuint index = glGetProgramResourceIndex(p.program, GL_SHADER_STORAGE_BLOCK, b.name.CStr());
                            if (index != GL_INVALID_INDEX)
                                glShaderStorageBlockBinding(p.program, index, b.slot);
                        }
                    }
                }
                appliedPipeline = {};
                return pipelines.Add(Move(p));
            }

            void Destroy(BufferHandle h) override
            {
                Buffer b;
                if (buffers.Remove(h, b))
                    glDeleteBuffers(1, &b.id);
            }

            void Destroy(TextureHandle h) override
            {
                Texture t;
                if (!textures.Remove(h, t))
                    return;
                glDeleteTextures(1, &t.id);
                Array<u64> stale;
                for (auto e : framebuffers)
                    if ((e.key & 0xFFFFFFFFull) == h.value || (e.key >> 32) == h.value)
                        stale.Push(e.key);
                for (u64 k : stale)
                {
                    glDeleteFramebuffers(1, framebuffers.Find(k));
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
                u32 aligned = u32(AlignUp(size, caps.uniformBufferAlignment));
                if (transientHead + aligned > transientSize)
                {
                    UploadTransient();
                    transientHead = transientUploaded = 0;
                    transientOrphan = true;
                }
                TransientAllocation a{transient, transientHead, size, transientCpu.Data() + transientHead};
                transientHead += aligned;
                return a;
            }

            void UploadTransient()
            {
                if (transientHead == transientUploaded)
                    return;
                glBindBuffer(GL_UNIFORM_BUFFER, transientId);
                if (transientOrphan)
                {
                    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(transientSize), nullptr, GL_STREAM_DRAW);
                    transientOrphan = false;
                }
                glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(transientUploaded), GLsizeiptr(transientHead - transientUploaded),
                                transientCpu.Data() + transientUploaded);
                transientUploaded = transientHead;
            }

            void BeginFrame() override { context->MakeCurrent(); }
            void EndFrame() override {}
            void Present() override { context->Swap(); }
            void Resize(u32, u32) override {}
            void WaitIdle() override { glFinish(); }

            GLuint FramebufferFor(const RenderPassDesc& rp)
            {
                u64 key = u64(rp.colors[0].texture.value) | (u64(rp.depth.texture.value) << 32);
                for (u32 i = 1; i < rp.colorCount; i++)
                    key = key * 1099511628211ull ^ rp.colors[i].texture.value;
                if (GLuint* existing = framebuffers.Find(key))
                    return *existing;
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                GLenum drawBuffers[kMaxColorTargets];
                for (u32 i = 0; i < rp.colorCount; i++)
                {
                    Texture* t = textures.Get(rp.colors[i].texture);
                    drawBuffers[i] = t ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
                    if (t)
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, t->id, GLint(rp.colors[i].mipLevel));
                }
                glDrawBuffers(GLsizei(rp.colorCount), drawBuffers);
                if (Texture* d = textures.Get(rp.depth.texture))
                    glFramebufferTexture2D(GL_FRAMEBUFFER, d->desc.format == Format::D24UnormS8Uint ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                           GL_TEXTURE_2D, d->id, 0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    HX_LOG_ERROR(GLLog, "incomplete framebuffer");
                framebuffers.Set(key, fbo);
                return fbo;
            }

            void ApplyPipeline(Pipeline* p)
            {
                const PipelineDesc& d = p->desc;
                glUseProgram(p->program);
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
                if (api.PolygonMode)
                    glPolygonMode(GL_FRONT_AND_BACK, d.wireframe ? GL_LINE : GL_FILL);
                attributesDirty = true;
            }

            void ApplyFrontFace()
            {
                bool flip = offscreen && caps.clipControl;
                glFrontFace((frontCounterClockwise != flip) ? GL_CCW : GL_CW);
            }

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
                UploadTransient();
                UploadBindless();
                glBindVertexArray(vao);
                commands.ForEach([&](CommandType type, const u8* data) {
                    switch (type)
                    {
                    case CommandType::BeginRenderPass:
                    {
                        const RenderPassDesc& rp = CommandData<CmdBeginRenderPass>(data).desc;
                        Texture* first = textures.Get(rp.colors[0].texture);
                        Texture* depth = textures.Get(rp.depth.texture);
                        offscreen = first || depth;
                        u32 width = 0, height = 0;
                        if (offscreen)
                        {
                            glBindFramebuffer(GL_FRAMEBUFFER, FramebufferFor(rp));
                            Texture* sizeSource = first ? first : depth;
                            width = sizeSource->desc.width;
                            height = sizeSource->desc.height;
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
                        pipeline = pipelines.Get(h);
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
                        VertexBinding next{b ? b->id : 0, c.offset};
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
                        indexOffset = c.offset;
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
                        if (attributesDirty || (c.firstInstance && !caps.baseInstance))
                            SetupAttributes(c.firstInstance);
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
                        if (attributesDirty || (c.firstInstance && !caps.baseInstance))
                            SetupAttributes(c.firstInstance);
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
                        if (Buffer* b = buffers.Get(c.buffer))
                        {
                            glBindBuffer(GL_COPY_WRITE_BUFFER, b->id);
                            glBufferSubData(GL_COPY_WRITE_BUFFER, GLintptr(c.offset), GLsizeiptr(c.size), data + sizeof(CmdUpdateBuffer));
                        }
                        break;
                    }
                    case CommandType::CopyBuffer:
                    {
                        const CmdCopyBuffer& c = CommandData<CmdCopyBuffer>(data);
                        Buffer* src = buffers.Get(c.source);
                        Buffer* dst = buffers.Get(c.destination);
                        if (src && dst)
                        {
                            glBindBuffer(GL_COPY_READ_BUFFER, src->id);
                            glBindBuffer(GL_COPY_WRITE_BUFFER, dst->id);
                            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, GLintptr(c.sourceOffset), GLintptr(c.destinationOffset), GLsizeiptr(c.size));
                        }
                        break;
                    }
                    case CommandType::CopyTextureToBuffer:
                        break;
                    }
                });
            }

            bool ReadTexture(TextureHandle handle, u32 mip, void* out, usize size) override
            {
                Texture* t = textures.Get(handle);
                if (!t || t->target != GL_TEXTURE_2D || IsDepthFormat(t->desc.format))
                    return false;
                u32 w = Max(1u, t->desc.width >> mip), h = Max(1u, t->desc.height >> mip), bpp = FormatBytesPerPixel(t->desc.format);
                usize row = usize(w) * bpp;
                if (size < row * h)
                    return false;
                GLFormat f = ToGL(t->desc.format);
                glBindFramebuffer(GL_FRAMEBUFFER, readFramebuffer);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, GLint(mip));
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
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
                glBindBuffer(GL_COPY_READ_BUFFER, b->id);
                if (api.GetBufferSubData)
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
