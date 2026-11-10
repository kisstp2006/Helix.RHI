#include <Helix/RHI/Bindless/Implementation/BindlessTable.h>
#include <Helix/RHI/Resources/Implementation/ResourcePool.h>
#include <Helix/RHI/Timing/Implementation/GpuTimerFrames.h>

#include <Helix/D3D11/D3D11.h>
#include <Helix/RHI/Device/Device.h>

#include <cstdio>
#include <cstring>

namespace helix::rhi
{
    using namespace helix::d3d11;

    namespace
    {
        DXGI_FORMAT ToDxgi(Format f)
        {
            switch (f)
            {
            case Format::R8Unorm: return DXGI_FORMAT_R8_UNORM;
            case Format::RG8Unorm: return DXGI_FORMAT_R8G8_UNORM;
            case Format::RGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case Format::RGBA8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case Format::BGRA8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case Format::BGRA8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            case Format::R16Float: return DXGI_FORMAT_R16_FLOAT;
            case Format::RG16Float: return DXGI_FORMAT_R16G16_FLOAT;
            case Format::RGBA16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case Format::R32Float: return DXGI_FORMAT_R32_FLOAT;
            case Format::RG32Float: return DXGI_FORMAT_R32G32_FLOAT;
            case Format::RGB32Float: return DXGI_FORMAT_R32G32B32_FLOAT;
            case Format::RGBA32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case Format::R32Uint: return DXGI_FORMAT_R32_UINT;
            case Format::RG32Uint: return DXGI_FORMAT_R32G32_UINT;
            case Format::RGBA32Uint: return DXGI_FORMAT_R32G32B32A32_UINT;
            case Format::RGB10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case Format::RG11B10Float: return DXGI_FORMAT_R11G11B10_FLOAT;
            case Format::D16Unorm: return DXGI_FORMAT_D16_UNORM;
            case Format::D24UnormS8Uint: return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case Format::D32Float: return DXGI_FORMAT_D32_FLOAT;
            case Format::BC1Unorm: return DXGI_FORMAT_BC1_UNORM;
            case Format::BC3Unorm: return DXGI_FORMAT_BC3_UNORM;
            case Format::BC5Unorm: return DXGI_FORMAT_BC5_UNORM;
            case Format::BC7Unorm: return DXGI_FORMAT_BC7_UNORM;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        DXGI_FORMAT ToVertexFormat(VertexFormat f)
        {
            switch (f)
            {
            case VertexFormat::Float1: return DXGI_FORMAT_R32_FLOAT;
            case VertexFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
            case VertexFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
            case VertexFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case VertexFormat::UByte4Norm: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case VertexFormat::UInt1: return DXGI_FORMAT_R32_UINT;
            }
            return DXGI_FORMAT_UNKNOWN;
        }

        D3D11_COMPARISON_FUNC ToCompare(CompareFunction f) { return D3D11_COMPARISON_FUNC(int(f) + 1); }

        D3D11_BLEND ToBlend(BlendFactor f)
        {
            static const D3D11_BLEND map[] = {D3D11_BLEND_ZERO,      D3D11_BLEND_ONE,           D3D11_BLEND_SRC_COLOR,  D3D11_BLEND_INV_SRC_COLOR,
                                              D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
                                              D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA};
            return map[int(f)];
        }

        D3D11_BLEND_OP ToBlendOp(BlendOp op) { return D3D11_BLEND_OP(int(op) + 1); }

        D3D11_TEXTURE_ADDRESS_MODE ToAddress(AddressMode m)
        {
            static const D3D11_TEXTURE_ADDRESS_MODE map[] = {D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP,
                                                             D3D11_TEXTURE_ADDRESS_BORDER};
            return map[int(m)];
        }

        D3D11_PRIMITIVE_TOPOLOGY ToTopology(PrimitiveTopology t)
        {
            static const D3D11_PRIMITIVE_TOPOLOGY map[] = {D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
                                                           D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
                                                           D3D11_PRIMITIVE_TOPOLOGY_POINTLIST};
            return map[int(t)];
        }

        struct Buffer
        {
            ID3D11Buffer* buffer = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            ID3D11UnorderedAccessView* uav = nullptr;
            BufferDesc desc{};
        };

        struct Texture
        {
            i32 pool = -1;
            u32 slice = 0;
            ID3D11Resource* resource = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            ID3D11RenderTargetView* rtv = nullptr;
            ID3D11DepthStencilView* dsv = nullptr;
            ID3D11UnorderedAccessView* uavs[16] = {};
            TextureDesc desc{};
        };

        struct Sampler
        {
            ID3D11SamplerState* state = nullptr;
        };

        struct Shader
        {
            ShaderStage stage{};
            ID3D11DeviceChild* object = nullptr;
            Array<u8> bytecode;
        };

        struct Pipeline
        {
            ID3D11VertexShader* vs = nullptr;
            ID3D11PixelShader* ps = nullptr;
            ID3D11ComputeShader* cs = nullptr;
            ID3D11InputLayout* layout = nullptr;
            ID3D11RasterizerState* raster = nullptr;
            ID3D11DepthStencilState* depth = nullptr;
            ID3D11BlendState* blend = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            u32 strides[kMaxVertexBuffers] = {};
        };

        template <typename T>
        void Release(T*& p)
        {
            if (p)
                p->Release();
            p = nullptr;
        }

        struct BindlessPool
        {
            ID3D11Texture2D* array = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            u32 width = 0, height = 0, mips = 0, capacity = 0, next = 0;
            Array<u32> freeSlices;
        };

        void ReleaseTexture(Texture& t)
        {
            for (auto& u : t.uavs)
                Release(u);
            Release(t.srv);
            Release(t.rtv);
            Release(t.dsv);
            Release(t.resource);
        }

        struct UniformBinding
        {
            ID3D11Buffer* buffer = nullptr;
            UINT first = 0, count = 0;
            bool ranged = false;
        };

        IDXGIAdapter1* HardwareAdapter(u32 index)
        {
            if (!LoadLibraries())
                return nullptr;
            IDXGIFactory2* factory = nullptr;
            if (Failed(CreateDXGIFactory2(0, IDXGIFactory2::Iid, reinterpret_cast<void**>(&factory))))
                return nullptr;
            IDXGIAdapter1* result = nullptr;
            u32 hardware = 0;
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; Succeeded(factory->EnumAdapters1(i, &adapter)); i++)
            {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && hardware++ == index)
                {
                    result = adapter;
                    break;
                }
                Release(adapter);
            }
            Release(factory);
            return result;
        }

        class D3D11Device final : public Device
        {
        public:
            ID3D11Device* device = nullptr;
            ID3D11Device1* device1 = nullptr;
            ID3D11DeviceContext* context = nullptr;
            ID3D11DeviceContext1* context1 = nullptr;
            IDXGISwapChain1* swapChain = nullptr;
            ID3D11RenderTargetView* backBufferRtv = nullptr;
            u32 backBufferWidth = 0, backBufferHeight = 0;
            bool tearing = false;
            bool vsync = true;
            DeviceCaps caps{};
            Pool<Buffer, BufferHandle> buffers;
            Pool<Texture, TextureHandle> textures;
            Pool<Sampler, SamplerHandle> samplers;
            Pool<Shader, ShaderHandle> shaders;
            Pool<Pipeline, PipelineHandle> pipelines;

            BufferHandle transient;
            ID3D11Buffer* transientBuffer = nullptr;
            Array<u8> transientCpu;
            u32 transientSize = 0;
            u32 transientHead = 0;
            u32 transientUploaded = 0;
            ID3D11Buffer* slotBuffers[kMaxUniformSlots] = {};

            Array<BindlessPool> bindlessPools;
            SamplerHandle bindlessSamplers[kBindlessD3D11MaxSamplers] = {};
            u32 bindlessSamplerCount = 0;
            BindlessTable<u32> bindless;
            ID3D11Buffer* bindlessBuffer = nullptr;
            ID3D11ShaderResourceView* bindlessSrv = nullptr;
            u32 bindlessCapacity = 0;

            // Compute state is applied at dispatch, read-only storage views before draws; UAVs are bound only around a dispatch.
            ID3D11UnorderedAccessView* storageUavs[kMaxStorageSlots] = {};
            ID3D11ShaderResourceView* storageSrvs[kMaxStorageSlots] = {};
            bool storageSrvsDirty = false;
            UniformBinding uniforms[kMaxUniformSlots];
            ID3D11ShaderResourceView* textureSrvs[kMaxTextureSlots] = {};
            ID3D11SamplerState* textureSamplers[kMaxTextureSlots] = {};
            Pipeline* computePipeline = nullptr;

            GpuTimerFrames timers;
            Array<ID3D11Query*> timestampQueries[GpuTimerFrames::kFrames];
            ID3D11Query* disjointQueries[GpuTimerFrames::kFrames] = {};

            bool Init(const DeviceDesc& desc)
            {
                if (!LoadLibraries())
                    return false;
                vsync = desc.vsync;
                const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
                UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (desc.debug ? D3D11_CREATE_DEVICE_DEBUG : 0);
                D3D_FEATURE_LEVEL level{};
                IDXGIAdapter1* chosen = desc.adapter >= 0 ? HardwareAdapter(u32(desc.adapter)) : nullptr;
                if (desc.adapter >= 0 && !chosen)
                    return false;
                D3D_DRIVER_TYPE driver = chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
                HRESULT hr = D3D11CreateDevice(chosen, driver, nullptr, flags, levels, 4, D3D11_SDK_VERSION, &device, &level, &context);
                if (Failed(hr) && desc.debug)
                    hr = D3D11CreateDevice(chosen, driver, nullptr, flags & ~UINT(D3D11_CREATE_DEVICE_DEBUG), levels, 4,
                                           D3D11_SDK_VERSION, &device, &level, &context);
                Release(chosen);
                if (Failed(hr) && desc.adapter < 0)
                    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
                                           &device, &level, &context);
                if (Failed(hr))
                    return false;
                device->QueryInterface(&device1);
                context->QueryInterface(&context1);

                caps.backend = Backend::D3D11;
                caps.apiMajor = 11;
                caps.apiMinor = level >= D3D_FEATURE_LEVEL_11_1 ? 1 : 0;
                caps.compute = level >= D3D_FEATURE_LEVEL_11_0;
                caps.storageBuffers = caps.compute;
                caps.drawIndirect = caps.compute;
                caps.gpuTimers = true;
                caps.baseInstance = true;
                caps.clipControl = true;
                caps.textureCompressionBC = true;
                caps.maxTextureSize = level >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;
                D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
                if (Succeeded(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))))
                    caps.uniformBufferOffsets = context1 && options.ConstantBufferOffsetting && options.MapNoOverwriteOnDynamicConstantBuffer;
                caps.uniformBufferAlignment = 256;
                caps.bindlessTextures = level >= D3D_FEATURE_LEVEL_11_0;
                caps.bindlessEmulated = true;
                caps.maxBindlessTextures = caps.bindlessTextures ? kBindlessD3D11MaxPools * kBindlessD3D11MaxSlices : 0;

                IDXGIDevice* dxgiDevice = nullptr;
                IDXGIAdapter* adapter = nullptr;
                if (Succeeded(device->QueryInterface(&dxgiDevice)) && Succeeded(dxgiDevice->GetAdapter(&adapter)))
                {
                    DXGI_ADAPTER_DESC ad{};
                    adapter->GetDesc(&ad);
                    std::snprintf(caps.renderer, sizeof(caps.renderer), "%ls", ad.Description);
                }
                Release(adapter);
                Release(dxgiDevice);

                transientSize = desc.transientBufferSize;
                transientCpu.Resize(transientSize);
                D3D11_BUFFER_DESC bd{transientSize, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
                if (Failed(device->CreateBuffer(&bd, nullptr, &transientBuffer)))
                    return false;
                transient = buffers.Add(Buffer{transientBuffer, nullptr, nullptr, BufferDesc{transientSize, BufferUniform, MemoryType::Upload}});
                if (!caps.uniformBufferOffsets)
                {
                    for (auto& b : slotBuffers)
                    {
                        D3D11_BUFFER_DESC sd{65536, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
                        device->CreateBuffer(&sd, nullptr, &b);
                    }
                }
                if (desc.window.window)
                    return CreateSwapChain(static_cast<HWND>(desc.window.window));
                return true;
            }

            bool CreateSwapChain(HWND hwnd)
            {
                IDXGIFactory2* factory = nullptr;
                if (Failed(CreateDXGIFactory2(0, IDXGIFactory2::Iid, reinterpret_cast<void**>(&factory))))
                    return false;
                IDXGIFactory5* factory5 = nullptr;
                if (Succeeded(factory->QueryInterface(&factory5)))
                {
                    BOOL allow = 0;
                    if (Succeeded(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                        tearing = allow != 0;
                    Release(factory5);
                }
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                sd.SampleDesc = {1, 0};
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.Scaling = DXGI_SCALING_NONE;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
                HRESULT hr = factory->CreateSwapChainForHwnd(device, hwnd, &sd, nullptr, nullptr, &swapChain);
                factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
                Release(factory);
                if (Failed(hr))
                    return false;
                return CreateBackBufferView();
            }

            bool CreateBackBufferView()
            {
                ID3D11Texture2D* backBuffer = nullptr;
                if (Failed(swapChain->GetBuffer(0, ID3D11Texture2D::Iid, reinterpret_cast<void**>(&backBuffer))))
                    return false;
                D3D11_TEXTURE2D_DESC td{};
                backBuffer->GetDesc(&td);
                backBufferWidth = td.Width;
                backBufferHeight = td.Height;
                HRESULT hr = device->CreateRenderTargetView(backBuffer, nullptr, &backBufferRtv);
                Release(backBuffer);
                return Succeeded(hr);
            }

            void Shutdown()
            {
                if (context)
                    context->ClearState();
                buffers.ForEach([](Buffer& b) {
                    Release(b.uav);
                    Release(b.srv);
                    Release(b.buffer);
                });
                textures.ForEach([](Texture& t) { ReleaseTexture(t); });
                for (u32 i = 0; i < GpuTimerFrames::kFrames; i++)
                {
                    for (ID3D11Query*& q : timestampQueries[i])
                        Release(q);
                    Release(disjointQueries[i]);
                }
                samplers.ForEach([](Sampler& s) { Release(s.state); });
                shaders.ForEach([](Shader& s) { Release(s.object); });
                pipelines.ForEach([this](Pipeline& p) { ReleasePipeline(p); });
                for (auto& b : slotBuffers)
                    Release(b);
                for (BindlessPool& p : bindlessPools)
                {
                    Release(p.srv);
                    Release(p.array);
                }
                Release(bindlessSrv);
                Release(bindlessBuffer);
                Release(backBufferRtv);
                Release(swapChain);
                Release(context1);
                Release(context);
                Release(device1);
                Release(device);
            }

            static void ReleasePipeline(Pipeline& p)
            {
                Release(p.layout);
                Release(p.raster);
                Release(p.depth);
                Release(p.blend);
                Release(p.vs);
                Release(p.ps);
                Release(p.cs);
            }

            const DeviceCaps& Caps() const override { return caps; }

            BufferHandle CreateBuffer(const BufferDesc& desc) override
            {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = UINT(desc.usage & BufferUniform ? AlignUp(desc.size, 16) : desc.size);
                bd.Usage = desc.memory == MemoryType::Upload ? D3D11_USAGE_DYNAMIC : desc.memory == MemoryType::Readback ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
                if (desc.memory != MemoryType::Readback)
                {
                    bd.BindFlags = (desc.usage & BufferVertex ? D3D11_BIND_VERTEX_BUFFER : 0) | (desc.usage & BufferIndex ? D3D11_BIND_INDEX_BUFFER : 0) |
                                   (desc.usage & BufferUniform ? D3D11_BIND_CONSTANT_BUFFER : 0) |
                                   (desc.usage & BufferStorage ? D3D11_BIND_SHADER_RESOURCE | (desc.memory == MemoryType::GpuOnly ? D3D11_BIND_UNORDERED_ACCESS : 0) : 0);
                }
                bd.CPUAccessFlags = desc.memory == MemoryType::Upload ? D3D11_CPU_ACCESS_WRITE : desc.memory == MemoryType::Readback ? D3D11_CPU_ACCESS_READ : 0;
                if (desc.usage & BufferIndirect)
                    bd.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
                // Structured views cannot be combined with vertex, index or indirect use, those buffers get raw views.
                bool structured = desc.structureStride && !(desc.usage & (BufferVertex | BufferIndex | BufferIndirect));
                if (desc.usage & BufferStorage)
                {
                    bd.MiscFlags |= structured ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
                    bd.StructureByteStride = structured ? desc.structureStride : 0;
                }
                D3D11_SUBRESOURCE_DATA init{desc.initialData, 0, 0};
                Buffer b;
                b.desc = desc;
                b.desc.initialData = nullptr;
                if (Failed(device->CreateBuffer(&bd, desc.initialData ? &init : nullptr, &b.buffer)))
                    return {};
                if (bd.BindFlags & D3D11_BIND_SHADER_RESOURCE && desc.usage & BufferStorage)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                    sd.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
                    sd.BufferEx = {0, UINT(structured ? desc.size / desc.structureStride : desc.size / 4), structured ? 0u : UINT(D3D11_BUFFEREX_SRV_FLAG_RAW)};
                    device->CreateShaderResourceView(b.buffer, &sd, &b.srv);
                }
                if (bd.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
                {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                    ud.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                    ud.Buffer = {0, UINT(structured ? desc.size / desc.structureStride : desc.size / 4), structured ? 0u : UINT(D3D11_BUFFER_UAV_FLAG_RAW)};
                    device->CreateUnorderedAccessView(b.buffer, &ud, &b.uav);
                }
                return buffers.Add(Move(b));
            }

            bool GrowPool(BindlessPool& p, u32 capacity)
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = p.width;
                td.Height = p.height;
                td.MipLevels = p.mips;
                td.ArraySize = capacity;
                td.Format = p.format;
                td.SampleDesc = {1, 0};
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                ID3D11Texture2D* array = nullptr;
                if (Failed(device->CreateTexture2D(&td, nullptr, &array)))
                    return false;
                if (p.array)
                {
                    for (u32 slice = 0; slice < p.next; slice++)
                        for (u32 mip = 0; mip < p.mips; mip++)
                            context->CopySubresourceRegion(array, mip + slice * p.mips, 0, 0, 0, p.array, mip + slice * p.mips, nullptr);
                }
                ID3D11ShaderResourceView* srv = nullptr;
                if (Failed(device->CreateShaderResourceView(array, nullptr, &srv)))
                {
                    Release(array);
                    return false;
                }
                Release(p.srv);
                Release(p.array);
                p.array = array;
                p.srv = srv;
                p.capacity = capacity;
                return true;
            }

            TextureHandle CreatePooledTexture(const TextureDesc& desc)
            {
                DXGI_FORMAT format = ToDxgi(desc.format);
                i32 index = -1;
                for (usize i = 0; i < bindlessPools.Size(); i++)
                {
                    const BindlessPool& p = bindlessPools[i];
                    if (p.format == format && p.width == desc.width && p.height == desc.height && p.mips == desc.mipLevels)
                        index = i32(i);
                }
                if (index < 0)
                {
                    if (bindlessPools.Size() == kBindlessD3D11MaxPools)
                        return {};
                    BindlessPool& p = bindlessPools.Emplace();
                    p.format = format;
                    p.width = desc.width;
                    p.height = desc.height;
                    p.mips = desc.mipLevels;
                    index = i32(bindlessPools.Size() - 1);
                }
                BindlessPool& p = bindlessPools[usize(index)];
                u32 slice;
                if (!p.freeSlices.Empty())
                {
                    slice = p.freeSlices.Back();
                    p.freeSlices.Pop();
                }
                else
                {
                    if (p.next == p.capacity)
                    {
                        u32 grown = Min(kBindlessD3D11MaxSlices, Max(16u, p.capacity * 2));
                        if (grown == p.capacity || !GrowPool(p, grown))
                            return {};
                    }
                    slice = p.next++;
                }
                if (desc.initialData)
                {
                    u32 pitch = desc.width * FormatBytesPerPixel(desc.format);
                    context->UpdateSubresource(p.array, slice * p.mips, nullptr, desc.initialData, pitch, pitch * desc.height);
                }
                Texture t;
                t.desc = desc;
                t.desc.initialData = nullptr;
                t.pool = index;
                t.slice = slice;
                return textures.Add(Move(t));
            }

            TextureHandle CreateTexture(const TextureDesc& desc) override
            {
                if (desc.bindless && caps.bindlessTextures && desc.type == TextureType::Texture2D && desc.sampleCount == 1 &&
                    desc.usage == TextureSampled && !IsCompressedFormat(desc.format))
                    return CreatePooledTexture(desc);
                Texture t;
                t.desc = desc;
                t.desc.initialData = nullptr;
                bool depth = IsDepthFormat(desc.format);
                UINT bind = (desc.usage & TextureSampled && !depth ? D3D11_BIND_SHADER_RESOURCE : 0) | (desc.usage & TextureRenderTarget ? D3D11_BIND_RENDER_TARGET : 0) |
                            (desc.usage & TextureDepthStencil ? D3D11_BIND_DEPTH_STENCIL : 0) | (desc.usage & TextureStorage ? D3D11_BIND_UNORDERED_ACCESS : 0);
                u32 bpp = FormatBytesPerPixel(desc.format);
                D3D11_SUBRESOURCE_DATA init[6] = {};
                for (u32 i = 0; i < 6; i++)
                    init[i] = {static_cast<const u8*>(desc.initialData) + usize(i) * desc.width * desc.height * bpp, desc.width * bpp, desc.width * desc.height * bpp};
                if (desc.type == TextureType::Texture3D)
                {
                    D3D11_TEXTURE3D_DESC td{desc.width, desc.height, desc.depthOrLayers, desc.mipLevels, ToDxgi(desc.format), D3D11_USAGE_DEFAULT, bind, 0, 0};
                    ID3D11Texture3D* tex = nullptr;
                    if (Failed(device->CreateTexture3D(&td, desc.initialData && desc.mipLevels == 1 ? init : nullptr, &tex)))
                        return {};
                    t.resource = tex;
                }
                else
                {
                    bool cube = desc.type == TextureType::TextureCube;
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = desc.width;
                    td.Height = desc.height;
                    td.MipLevels = desc.mipLevels;
                    td.ArraySize = cube ? 6 : desc.depthOrLayers;
                    td.Format = ToDxgi(desc.format);
                    td.SampleDesc = {desc.sampleCount, 0};
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = bind;
                    td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
                    ID3D11Texture2D* tex = nullptr;
                    bool withData = desc.initialData && desc.mipLevels == 1;
                    if (Failed(device->CreateTexture2D(&td, withData ? init : nullptr, &tex)))
                        return {};
                    t.resource = tex;
                }
                if (bind & D3D11_BIND_SHADER_RESOURCE)
                    device->CreateShaderResourceView(t.resource, nullptr, &t.srv);
                if (bind & D3D11_BIND_RENDER_TARGET)
                    device->CreateRenderTargetView(t.resource, nullptr, &t.rtv);
                if (bind & D3D11_BIND_DEPTH_STENCIL)
                    device->CreateDepthStencilView(t.resource, nullptr, &t.dsv);
                return textures.Add(Move(t));
            }

            SamplerHandle CreateSampler(const SamplerDesc& desc) override
            {
                D3D11_SAMPLER_DESC sd{};
                if (desc.maxAnisotropy > 1)
                    sd.Filter = desc.compare ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
                else
                {
                    int f = (desc.minFilter == Filter::Linear ? 0x10 : 0) | (desc.magFilter == Filter::Linear ? 0x4 : 0) | (desc.mipFilter == Filter::Linear ? 0x1 : 0);
                    sd.Filter = D3D11_FILTER(f | (desc.compare ? 0x80 : 0));
                }
                sd.AddressU = ToAddress(desc.addressU);
                sd.AddressV = ToAddress(desc.addressV);
                sd.AddressW = ToAddress(desc.addressW);
                sd.MipLODBias = desc.lodBias;
                sd.MaxAnisotropy = desc.maxAnisotropy;
                sd.ComparisonFunc = desc.compare ? ToCompare(desc.compareFunction) : D3D11_COMPARISON_NEVER;
                sd.MinLOD = desc.minLod;
                sd.MaxLOD = desc.maxLod;
                Sampler s;
                if (Failed(device->CreateSamplerState(&sd, &s.state)))
                    return {};
                return samplers.Add(Move(s));
            }

            ShaderHandle CreateShader(const ShaderDesc& desc) override
            {
                if (!desc.bytecode)
                    return {};
                Shader s;
                s.stage = desc.stage;
                HRESULT hr = E_FAIL;
                if (desc.stage == ShaderStage::Vertex)
                {
                    ID3D11VertexShader* vs = nullptr;
                    hr = device->CreateVertexShader(desc.bytecode, desc.bytecodeSize, nullptr, &vs);
                    s.object = vs;
                    s.bytecode.Resize(desc.bytecodeSize);
                    std::memcpy(s.bytecode.Data(), desc.bytecode, desc.bytecodeSize);
                }
                else if (desc.stage == ShaderStage::Fragment)
                {
                    ID3D11PixelShader* ps = nullptr;
                    hr = device->CreatePixelShader(desc.bytecode, desc.bytecodeSize, nullptr, &ps);
                    s.object = ps;
                }
                else
                {
                    ID3D11ComputeShader* cs = nullptr;
                    hr = device->CreateComputeShader(desc.bytecode, desc.bytecodeSize, nullptr, &cs);
                    s.object = cs;
                }
                if (Failed(hr))
                    return {};
                return shaders.Add(Move(s));
            }

            PipelineHandle CreatePipeline(const PipelineDesc& desc) override
            {
                Shader* vs = shaders.Get(desc.vertexShader);
                Shader* ps = shaders.Get(desc.fragmentShader);
                if (!vs || !ps)
                    return {};
                Pipeline p;
                p.vs = static_cast<ID3D11VertexShader*>(vs->object);
                p.ps = static_cast<ID3D11PixelShader*>(ps->object);
                p.vs->AddRef();
                p.ps->AddRef();
                p.topology = ToTopology(desc.topology);
                D3D11_INPUT_ELEMENT_DESC elements[kMaxVertexAttributes];
                for (u32 i = 0; i < desc.attributeCount; i++)
                {
                    const VertexAttribute& a = desc.attributes[i];
                    bool instanced = desc.vertexBuffers[a.bufferSlot].step == StepMode::Instance;
                    elements[i] = {"TEXCOORD", a.location, ToVertexFormat(a.format), a.bufferSlot, a.offset,
                                   instanced ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA, instanced ? 1u : 0u};
                }
                for (u32 i = 0; i < desc.vertexBufferCount; i++)
                    p.strides[i] = desc.vertexBuffers[i].stride;
                if (desc.attributeCount &&
                    Failed(device->CreateInputLayout(elements, desc.attributeCount, vs->bytecode.Data(), vs->bytecode.Size(), &p.layout)))
                {
                    ReleasePipeline(p);
                    return {};
                }
                D3D11_RASTERIZER_DESC rd{};
                rd.FillMode = desc.wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
                rd.CullMode = D3D11_CULL_MODE(int(desc.cull) + 1);
                rd.FrontCounterClockwise = desc.frontCounterClockwise;
                rd.DepthClipEnable = 1;
                rd.ScissorEnable = 1;
                rd.MultisampleEnable = desc.sampleCount > 1;
                device->CreateRasterizerState(&rd, &p.raster);
                D3D11_DEPTH_STENCIL_DESC dd{};
                dd.DepthEnable = desc.depthTest || desc.depthWrite;
                dd.DepthWriteMask = desc.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
                dd.DepthFunc = desc.depthTest ? ToCompare(desc.depthCompare) : D3D11_COMPARISON_ALWAYS;
                dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
                device->CreateDepthStencilState(&dd, &p.depth);
                D3D11_BLEND_DESC bd{};
                bd.IndependentBlendEnable = 1;
                for (u32 i = 0; i < kMaxColorTargets; i++)
                {
                    const BlendState& b = desc.blend[i];
                    bd.RenderTarget[i] = {b.enabled, ToBlend(b.srcColor), ToBlend(b.dstColor), ToBlendOp(b.colorOp), ToBlend(b.srcAlpha),
                                          ToBlend(b.dstAlpha), ToBlendOp(b.alphaOp), UINT8(b.writeMask)};
                }
                device->CreateBlendState(&bd, &p.blend);
                return pipelines.Add(Move(p));
            }

            PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override
            {
                Shader* cs = shaders.Get(desc.computeShader);
                if (!cs || cs->stage != ShaderStage::Compute || !caps.compute)
                    return {};
                Pipeline p;
                p.cs = static_cast<ID3D11ComputeShader*>(cs->object);
                p.cs->AddRef();
                return pipelines.Add(Move(p));
            }

            void Destroy(BufferHandle h) override
            {
                Buffer b;
                if (buffers.Remove(h, b))
                {
                    Release(b.uav);
                    Release(b.srv);
                    Release(b.buffer);
                }
            }

            void Destroy(TextureHandle h) override
            {
                Texture t;
                if (textures.Remove(h, t))
                {
                    if (t.pool >= 0)
                        bindlessPools[usize(t.pool)].freeSlices.Push(t.slice);
                    ReleaseTexture(t);
                }
            }

            void Destroy(SamplerHandle h) override
            {
                Sampler s;
                if (samplers.Remove(h, s))
                    Release(s.state);
            }

            void Destroy(ShaderHandle h) override
            {
                Shader s;
                if (shaders.Remove(h, s))
                    Release(s.object);
            }

            void Destroy(PipelineHandle h) override
            {
                Pipeline p;
                if (pipelines.Remove(h, p))
                    ReleasePipeline(p);
            }

            u32 RegisterBindless(TextureHandle texture, SamplerHandle sampler) override
            {
                Texture* t = textures.Get(texture);
                if (!caps.bindlessTextures || !t || t->pool < 0 || !samplers.Get(sampler))
                    return kInvalidBindlessIndex;
                u32 slot = bindlessSamplerCount;
                for (u32 i = 0; i < bindlessSamplerCount; i++)
                    if (bindlessSamplers[i] == sampler)
                        slot = i;
                if (slot == bindlessSamplerCount)
                {
                    if (bindlessSamplerCount == kBindlessD3D11MaxSamplers)
                        return kInvalidBindlessIndex;
                    bindlessSamplers[bindlessSamplerCount++] = sampler;
                }
                return bindless.Add(PackD3D11BindlessEntry(u32(t->pool), t->slice, slot));
            }

            void UnregisterBindless(u32 index) override
            {
                u32 entry;
                bindless.Remove(index, entry);
            }

            void UploadBindless()
            {
                if (bindless.Size() == 0)
                    return;
                if (bindless.Dirty())
                {
                    if (bindless.Size() > bindlessCapacity)
                    {
                        Release(bindlessSrv);
                        Release(bindlessBuffer);
                        bindlessCapacity = Max(bindless.Size() * 2, 1024u);
                        D3D11_BUFFER_DESC bd{bindlessCapacity * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
                        if (Failed(device->CreateBuffer(&bd, nullptr, &bindlessBuffer)) ||
                            Failed(device->CreateShaderResourceView(bindlessBuffer, nullptr, &bindlessSrv)))
                            return;
                    }
                    D3D11_BOX box{0, 0, 0, bindless.Size() * 4, 1, 1};
                    context->UpdateSubresource(bindlessBuffer, 0, &box, bindless.Data(), 0, 0);
                    bindless.ClearDirty();
                }
                ID3D11ShaderResourceView* pools[kBindlessD3D11MaxPools] = {};
                for (usize i = 0; i < bindlessPools.Size(); i++)
                    pools[i] = bindlessPools[i].srv;
                ID3D11SamplerState* states[kBindlessD3D11MaxSamplers] = {};
                for (u32 i = 0; i < bindlessSamplerCount; i++)
                    if (Sampler* sampler = samplers.Get(bindlessSamplers[i]))
                        states[i] = sampler->state;
                using SetSrv = void (ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView* const*);
                using SetSamplers = void (ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState* const*);
                const SetSrv srvSetters[] = {&ID3D11DeviceContext::VSSetShaderResources, &ID3D11DeviceContext::PSSetShaderResources,
                                             &ID3D11DeviceContext::CSSetShaderResources};
                const SetSamplers samplerSetters[] = {&ID3D11DeviceContext::VSSetSamplers, &ID3D11DeviceContext::PSSetSamplers,
                                                      &ID3D11DeviceContext::CSSetSamplers};
                for (int stage = 0; stage < (caps.compute ? 3 : 2); stage++)
                {
                    SetSrv setSrv = srvSetters[stage];
                    SetSamplers setSamplers = samplerSetters[stage];
                    (context->*setSrv)(kBindlessD3D11TableRegister, 1, &bindlessSrv);
                    (context->*setSrv)(kBindlessD3D11PoolRegister, kBindlessD3D11MaxPools, pools);
                    (context->*setSamplers)(kBindlessD3D11SamplerRegister, kBindlessD3D11MaxSamplers, states);
                }
            }

            TransientAllocation AllocateTransient(u32 size) override
            {
                u32 aligned = u32(AlignUp(size, 256));
                if (transientHead + aligned > transientSize)
                {
                    UploadTransient();
                    transientHead = transientUploaded = 0;
                    wrapped = true;
                }
                TransientAllocation a{transient, transientHead, size, transientCpu.Data() + transientHead};
                transientHead += aligned;
                return a;
            }

            bool wrapped = true;

            void UploadTransient()
            {
                if (transientHead == transientUploaded)
                    return;
                D3D11_MAPPED_SUBRESOURCE m{};
                if (Succeeded(context->Map(transientBuffer, 0, wrapped ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
                {
                    if (wrapped)
                        std::memcpy(m.pData, transientCpu.Data(), transientHead);
                    else
                        std::memcpy(static_cast<u8*>(m.pData) + transientUploaded, transientCpu.Data() + transientUploaded, transientHead - transientUploaded);
                    context->Unmap(transientBuffer, 0);
                }
                wrapped = false;
                transientUploaded = transientHead;
            }

            void BeginFrame() override {}

            void EndFrame() override
            {
                if (timers.Close())
                    context->End(disjointQueries[timers.CurrentIndex()]);
                PollTimers();
                timers.Advance();
            }

            void PollTimers()
            {
                timers.ForEachPending([&](u32 index, GpuTimerFrames::Frame& f) {
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
                    if (context->GetData(disjointQueries[index], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                        return false;
                    Array<u64> ticks;
                    ticks.Resize(f.queries);
                    for (u32 q = 0; q < f.queries; q++)
                        if (context->GetData(timestampQueries[index][q], &ticks[q], sizeof(u64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                            return false;
                    timers.Resolve(f, ticks.Data(), dj.Frequency, !dj.Disjoint);
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
                if (index < 0)
                    return;
                u32 frame = timers.CurrentIndex();
                Array<ID3D11Query*>& queries = timestampQueries[frame];
                D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP, 0};
                while (queries.Size() <= usize(index))
                {
                    ID3D11Query* q = nullptr;
                    device->CreateQuery(&qd, &q);
                    queries.Push(q);
                }
                if (index == 0)
                {
                    if (!disjointQueries[frame])
                    {
                        D3D11_QUERY_DESC dd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                        device->CreateQuery(&dd, &disjointQueries[frame]);
                    }
                    context->Begin(disjointQueries[frame]);
                }
                context->End(queries[usize(index)]);
            }

            void ApplyStorageViews()
            {
                if (!storageSrvsDirty)
                    return;
                context->VSSetShaderResources(kStorageD3D11ReadOnlyRegister, kMaxStorageSlots, storageSrvs);
                context->PSSetShaderResources(kStorageD3D11ReadOnlyRegister, kMaxStorageSlots, storageSrvs);
                storageSrvsDirty = false;
            }

            // Binds the compute view of the current state, runs the dispatch and unbinds the UAVs again so later passes can read the results.
            template <typename F>
            void RunCompute(F&& dispatch)
            {
                if (!computePipeline)
                    return;
                ID3D11Buffer* cbs[kMaxUniformSlots] = {};
                for (u32 i = 0; i < kMaxUniformSlots; i++)
                    cbs[i] = uniforms[i].buffer;
                context->CSSetConstantBuffers(0, kMaxUniformSlots, cbs);
                if (context1)
                    for (u32 i = 0; i < kMaxUniformSlots; i++)
                        if (uniforms[i].ranged)
                            context1->CSSetConstantBuffers1(i, 1, &uniforms[i].buffer, &uniforms[i].first, &uniforms[i].count);
                context->CSSetShaderResources(0, kMaxTextureSlots, textureSrvs);
                context->CSSetSamplers(0, kMaxTextureSlots, textureSamplers);
                context->CSSetShaderResources(kStorageD3D11ReadOnlyRegister, kMaxStorageSlots, storageSrvs);
                context->CSSetUnorderedAccessViews(0, kMaxStorageSlots, storageUavs, nullptr);
                dispatch();
                ID3D11UnorderedAccessView* none[kMaxStorageSlots] = {};
                context->CSSetUnorderedAccessViews(0, kMaxStorageSlots, none, nullptr);
                storageSrvsDirty = true;
            }

            ID3D11UnorderedAccessView* TextureUav(Texture& t, u32 mip)
            {
                if (mip >= 16 || !(t.desc.usage & TextureStorage) || !t.resource)
                    return nullptr;
                if (!t.uavs[mip])
                {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                    ud.Format = ToDxgi(t.desc.format);
                    if (t.desc.type == TextureType::Texture3D)
                    {
                        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
                        ud.Texture3D = {mip, 0, ~0u};
                    }
                    else if (t.desc.type == TextureType::Texture2D && t.desc.depthOrLayers == 1)
                    {
                        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                        ud.Texture2D = {mip};
                    }
                    else
                    {
                        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                        ud.Texture2DArray = {mip, 0, t.desc.type == TextureType::TextureCube ? 6u : t.desc.depthOrLayers};
                    }
                    device->CreateUnorderedAccessView(t.resource, &ud, &t.uavs[mip]);
                }
                return t.uavs[mip];
            }

            void Present() override
            {
                if (swapChain)
                    swapChain->Present(vsync ? 1 : 0, !vsync && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
            }

            void Resize(u32 width, u32 height) override
            {
                if (!swapChain || (width == backBufferWidth && height == backBufferHeight) || !width || !height)
                    return;
                context->OMSetRenderTargets(0, nullptr, nullptr);
                Release(backBufferRtv);
                swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                CreateBackBufferView();
            }

            void WaitIdle() override
            {
                D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
                ID3D11Query* q = nullptr;
                if (Failed(device->CreateQuery(&qd, &q)))
                    return;
                context->End(q);
                context->Flush();
                BOOL done = 0;
                while (context->GetData(q, &done, sizeof(done), 0) == S_FALSE)
                {
                }
                Release(q);
            }

            void BindUniform(u32 slot, Buffer* b, u32 offset, u32 size)
            {
                if (!b)
                    return;
                if (b->buffer == transientBuffer && !caps.uniformBufferOffsets)
                {
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (Succeeded(context->Map(slotBuffers[slot], 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                    {
                        std::memcpy(m.pData, transientCpu.Data() + offset, Min<u32>(size, 65536));
                        context->Unmap(slotBuffers[slot], 0);
                    }
                    context->VSSetConstantBuffers(slot, 1, &slotBuffers[slot]);
                    context->PSSetConstantBuffers(slot, 1, &slotBuffers[slot]);
                    uniforms[slot] = {slotBuffers[slot], 0, 0, false};
                    return;
                }
                if (context1 && (offset || caps.uniformBufferOffsets))
                {
                    UINT first = offset / 16, count = UINT(AlignUp(size, 256) / 16);
                    context1->VSSetConstantBuffers1(slot, 1, &b->buffer, &first, &count);
                    context1->PSSetConstantBuffers1(slot, 1, &b->buffer, &first, &count);
                    uniforms[slot] = {b->buffer, first, count, true};
                    return;
                }
                context->VSSetConstantBuffers(slot, 1, &b->buffer);
                context->PSSetConstantBuffers(slot, 1, &b->buffer);
                uniforms[slot] = {b->buffer, 0, 0, false};
            }

            void Submit(const CommandList& commands) override
            {
                UploadTransient();
                UploadBindless();
                Pipeline* pipeline = nullptr;
                commands.ForEach([&](CommandType type, const u8* data) {
                    switch (type)
                    {
                    case CommandType::BeginRenderPass:
                    {
                        const RenderPassDesc& rp = CommandData<CmdBeginRenderPass>(data).desc;
                        ID3D11RenderTargetView* rtvs[kMaxColorTargets] = {};
                        u32 width = backBufferWidth, height = backBufferHeight;
                        for (u32 i = 0; i < rp.colorCount; i++)
                        {
                            if (Texture* t = textures.Get(rp.colors[i].texture))
                            {
                                rtvs[i] = t->rtv;
                                width = t->desc.width;
                                height = t->desc.height;
                            }
                            else
                                rtvs[i] = backBufferRtv;
                        }
                        Texture* depth = textures.Get(rp.depth.texture);
                        if (depth)
                        {
                            width = depth->desc.width;
                            height = depth->desc.height;
                        }
                        context->OMSetRenderTargets(rp.colorCount, rtvs, depth ? depth->dsv : nullptr);
                        for (u32 i = 0; i < rp.colorCount; i++)
                            if (rp.colors[i].load == LoadOp::Clear && rtvs[i])
                                context->ClearRenderTargetView(rtvs[i], rp.colors[i].clearColor);
                        if (depth && rp.depth.load == LoadOp::Clear)
                            context->ClearDepthStencilView(depth->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, rp.depth.clearDepth, rp.depth.clearStencil);
                        D3D11_VIEWPORT vp{0, 0, FLOAT(width), FLOAT(height), 0, 1};
                        context->RSSetViewports(1, &vp);
                        D3D11_RECT sc{0, 0, LONG(width), LONG(height)};
                        context->RSSetScissorRects(1, &sc);
                        break;
                    }
                    case CommandType::EndRenderPass:
                        break;
                    case CommandType::SetViewport:
                    {
                        const Viewport& v = CommandData<CmdSetViewport>(data).viewport;
                        D3D11_VIEWPORT vp{v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth};
                        context->RSSetViewports(1, &vp);
                        break;
                    }
                    case CommandType::SetScissor:
                    {
                        const ScissorRect& r = CommandData<CmdSetScissor>(data).rect;
                        D3D11_RECT sc{r.x, r.y, LONG(r.x + i32(r.width)), LONG(r.y + i32(r.height))};
                        context->RSSetScissorRects(1, &sc);
                        break;
                    }
                    case CommandType::SetPipeline:
                    {
                        Pipeline* p = pipelines.Get(CommandData<CmdSetPipeline>(data).pipeline);
                        if (p && p->cs)
                        {
                            computePipeline = p;
                            context->CSSetShader(p->cs, nullptr, 0);
                            break;
                        }
                        pipeline = p;
                        if (!pipeline)
                            break;
                        context->IASetInputLayout(pipeline->layout);
                        context->IASetPrimitiveTopology(pipeline->topology);
                        context->VSSetShader(pipeline->vs, nullptr, 0);
                        context->PSSetShader(pipeline->ps, nullptr, 0);
                        context->RSSetState(pipeline->raster);
                        context->OMSetDepthStencilState(pipeline->depth, 0);
                        context->OMSetBlendState(pipeline->blend, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
                        break;
                    }
                    case CommandType::SetVertexBuffer:
                    {
                        const CmdSetVertexBuffer& c = CommandData<CmdSetVertexBuffer>(data);
                        Buffer* b = buffers.Get(c.buffer);
                        UINT stride = pipeline ? pipeline->strides[c.slot] : 0, offset = c.offset;
                        ID3D11Buffer* buffer = b ? b->buffer : nullptr;
                        context->IASetVertexBuffers(c.slot, 1, &buffer, &stride, &offset);
                        break;
                    }
                    case CommandType::SetIndexBuffer:
                    {
                        const CmdSetIndexBuffer& c = CommandData<CmdSetIndexBuffer>(data);
                        Buffer* b = buffers.Get(c.buffer);
                        context->IASetIndexBuffer(b ? b->buffer : nullptr, c.type == IndexType::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, c.offset);
                        break;
                    }
                    case CommandType::SetUniformBuffer:
                    {
                        const CmdSetUniformBuffer& c = CommandData<CmdSetUniformBuffer>(data);
                        BindUniform(c.slot, buffers.Get(c.buffer), c.offset, c.size);
                        break;
                    }
                    case CommandType::SetTexture:
                    {
                        const CmdSetTexture& c = CommandData<CmdSetTexture>(data);
                        Texture* t = textures.Get(c.texture);
                        Sampler* s = samplers.Get(c.sampler);
                        ID3D11ShaderResourceView* srv = t ? t->srv : nullptr;
                        ID3D11SamplerState* state = s ? s->state : nullptr;
                        context->PSSetShaderResources(c.slot, 1, &srv);
                        context->PSSetSamplers(c.slot, 1, &state);
                        context->VSSetShaderResources(c.slot, 1, &srv);
                        context->VSSetSamplers(c.slot, 1, &state);
                        textureSrvs[c.slot] = srv;
                        textureSamplers[c.slot] = state;
                        break;
                    }
                    case CommandType::Draw:
                    {
                        ApplyStorageViews();
                        const CmdDraw& c = CommandData<CmdDraw>(data);
                        if (c.instanceCount == 1 && c.firstInstance == 0)
                            context->Draw(c.vertexCount, c.firstVertex);
                        else
                            context->DrawInstanced(c.vertexCount, c.instanceCount, c.firstVertex, c.firstInstance);
                        break;
                    }
                    case CommandType::DrawIndexed:
                    {
                        ApplyStorageViews();
                        const CmdDrawIndexed& c = CommandData<CmdDrawIndexed>(data);
                        if (c.instanceCount == 1 && c.firstInstance == 0)
                            context->DrawIndexed(c.indexCount, c.firstIndex, c.baseVertex);
                        else
                            context->DrawIndexedInstanced(c.indexCount, c.instanceCount, c.firstIndex, c.baseVertex, c.firstInstance);
                        break;
                    }
                    case CommandType::UpdateBuffer:
                    {
                        const CmdUpdateBuffer& c = CommandData<CmdUpdateBuffer>(data);
                        Buffer* b = buffers.Get(c.buffer);
                        if (!b)
                            break;
                        const u8* bytes = data + sizeof(CmdUpdateBuffer);
                        if (b->desc.memory == MemoryType::Upload)
                        {
                            D3D11_MAPPED_SUBRESOURCE m{};
                            if (Succeeded(context->Map(b->buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                            {
                                std::memcpy(static_cast<u8*>(m.pData) + c.offset, bytes, c.size);
                                context->Unmap(b->buffer, 0);
                            }
                        }
                        else
                        {
                            D3D11_BOX box{c.offset, 0, 0, c.offset + c.size, 1, 1};
                            bool whole = b->desc.usage & BufferUniform;
                            context->UpdateSubresource(b->buffer, 0, whole ? nullptr : &box, bytes, 0, 0);
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
                            D3D11_BOX box{c.sourceOffset, 0, 0, c.sourceOffset + c.size, 1, 1};
                            context->CopySubresourceRegion(dst->buffer, 0, c.destinationOffset, 0, 0, src->buffer, 0, &box);
                        }
                        break;
                    }
                    case CommandType::CopyTextureToBuffer:
                        break;
                    case CommandType::SetStorageBuffer:
                    {
                        const CmdSetStorageBuffer& c = CommandData<CmdSetStorageBuffer>(data);
                        if (c.slot >= kMaxStorageSlots)
                            break;
                        Buffer* b = buffers.Get(c.buffer);
                        bool write = c.access == StorageAccess::ReadWrite;
                        storageUavs[c.slot] = write && b ? b->uav : nullptr;
                        storageSrvs[c.slot] = !write && b ? b->srv : nullptr;
                        storageSrvsDirty = true;
                        break;
                    }
                    case CommandType::SetStorageTexture:
                    {
                        const CmdSetStorageTexture& c = CommandData<CmdSetStorageTexture>(data);
                        Texture* t = textures.Get(c.texture);
                        if (c.slot < kMaxStorageSlots)
                            storageUavs[c.slot] = t ? TextureUav(*t, c.mipLevel) : nullptr;
                        break;
                    }
                    case CommandType::Dispatch:
                    {
                        const CmdDispatch& c = CommandData<CmdDispatch>(data);
                        RunCompute([&] { context->Dispatch(c.x, c.y, c.z); });
                        break;
                    }
                    case CommandType::DispatchIndirect:
                    {
                        const CmdIndirect& c = CommandData<CmdIndirect>(data);
                        if (Buffer* b = buffers.Get(c.buffer))
                            RunCompute([&] { context->DispatchIndirect(b->buffer, c.offset); });
                        break;
                    }
                    case CommandType::DrawIndirect:
                    case CommandType::DrawIndexedIndirect:
                    {
                        const CmdIndirect& c = CommandData<CmdIndirect>(data);
                        Buffer* b = buffers.Get(c.buffer);
                        if (!b)
                            break;
                        ApplyStorageViews();
                        for (u32 i = 0; i < c.drawCount; i++)
                        {
                            if (type == CommandType::DrawIndirect)
                                context->DrawInstancedIndirect(b->buffer, c.offset + i * c.stride);
                            else
                                context->DrawIndexedInstancedIndirect(b->buffer, c.offset + i * c.stride);
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
                    }
                });
            }

            bool ReadTexture(TextureHandle handle, u32 mip, void* out, usize size) override
            {
                Texture* t = textures.Get(handle);
                if (!t || t->desc.type == TextureType::Texture3D)
                    return false;
                u32 w = Max(1u, t->desc.width >> mip), h = Max(1u, t->desc.height >> mip), bpp = FormatBytesPerPixel(t->desc.format);
                if (size < usize(w) * h * bpp)
                    return false;
                D3D11_TEXTURE2D_DESC td{w, h, 1, 1, ToDxgi(t->desc.format), {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
                ID3D11Texture2D* staging = nullptr;
                if (Failed(device->CreateTexture2D(&td, nullptr, &staging)))
                    return false;
                context->CopySubresourceRegion(staging, 0, 0, 0, 0, t->resource, mip, nullptr);
                D3D11_MAPPED_SUBRESOURCE m{};
                bool ok = Succeeded(context->Map(staging, 0, D3D11_MAP_READ, 0, &m));
                if (ok)
                {
                    for (u32 y = 0; y < h; y++)
                        std::memcpy(static_cast<u8*>(out) + usize(y) * w * bpp, static_cast<const u8*>(m.pData) + usize(y) * m.RowPitch, usize(w) * bpp);
                    context->Unmap(staging, 0);
                }
                Release(staging);
                return ok;
            }

            bool ReadBuffer(BufferHandle handle, u32 offset, void* out, u32 size) override
            {
                Buffer* b = buffers.Get(handle);
                if (!b)
                    return false;
                D3D11_BUFFER_DESC bd{size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
                ID3D11Buffer* staging = nullptr;
                if (Failed(device->CreateBuffer(&bd, nullptr, &staging)))
                    return false;
                D3D11_BOX box{offset, 0, 0, offset + size, 1, 1};
                context->CopySubresourceRegion(staging, 0, 0, 0, 0, b->buffer, 0, &box);
                D3D11_MAPPED_SUBRESOURCE m{};
                bool ok = Succeeded(context->Map(staging, 0, D3D11_MAP_READ, 0, &m));
                if (ok)
                {
                    std::memcpy(out, m.pData, size);
                    context->Unmap(staging, 0);
                }
                Release(staging);
                return ok;
            }

            void DestroySelf() override
            {
                Shutdown();
                Delete(HeapAllocator(), this);
            }
        };
    }

    bool D3D11Available() { return LoadLibraries(); }

    bool D3D11AdapterName(u32 index, char* out, usize size)
    {
        IDXGIAdapter1* adapter = HardwareAdapter(index);
        if (!adapter)
            return false;
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        std::snprintf(out, size, "%ls", d.Description);
        Release(adapter);
        return true;
    }

    Device* CreateD3D11Device(const DeviceDesc& desc)
    {
        auto* d = New<D3D11Device>(HeapAllocator());
        if (!d->Init(desc))
        {
            d->Shutdown();
            Delete(HeapAllocator(), d);
            return nullptr;
        }
        return d;
    }
}
