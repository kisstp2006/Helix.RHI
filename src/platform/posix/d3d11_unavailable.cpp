#include <helix/rhi/device.h>

namespace helix::rhi
{
    bool D3D11Available() { return false; }
    bool D3D11AdapterName(u32, char*, usize) { return false; }
    Device* CreateD3D11Device(const DeviceDesc&) { return nullptr; }
}
