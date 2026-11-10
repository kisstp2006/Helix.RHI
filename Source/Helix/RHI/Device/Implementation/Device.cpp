#include <Helix/RHI/Device/Device.h>

namespace helix::rhi
{
    Device* CreateD3D11Device(const DeviceDesc& desc);
    Device* CreateGLDevice(const DeviceDesc& desc);
    bool D3D11Available();
    bool GLAvailable(Backend backend);
    bool D3D11AdapterName(u32 index, char* out, usize size);

    bool Device::AdapterName(Backend backend, u32 index, char* out, usize size)
    {
        if (backend == Backend::D3D11)
            return D3D11AdapterName(index, out, size);
        return false;
    }

    bool Device::IsBackendAvailable(Backend backend)
    {
        if (backend == Backend::D3D11)
            return D3D11Available();
        return GLAvailable(backend);
    }

    Device* Device::Create(const DeviceDesc& desc)
    {
        if (desc.backend == Backend::D3D11)
            return CreateD3D11Device(desc);
        return CreateGLDevice(desc);
    }

    void Device::Destroy(Device* device)
    {
        if (device)
            device->DestroySelf();
    }
}
