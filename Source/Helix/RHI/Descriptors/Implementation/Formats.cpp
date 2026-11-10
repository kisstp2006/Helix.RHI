#include <Helix/RHI/Descriptors/Descriptors.h>

namespace helix::rhi
{
    bool IsDepthFormat(Format f) { return f == Format::D16Unorm || f == Format::D24UnormS8Uint || f == Format::D32Float; }

    bool IsCompressedFormat(Format f) { return f == Format::BC1Unorm || f == Format::BC3Unorm || f == Format::BC5Unorm || f == Format::BC7Unorm; }

    u32 FormatBytesPerPixel(Format f)
    {
        switch (f)
        {
        case Format::R8Unorm: return 1;
        case Format::RG8Unorm:
        case Format::R16Float:
        case Format::D16Unorm: return 2;
        case Format::RGBA8Unorm:
        case Format::RGBA8Srgb:
        case Format::BGRA8Unorm:
        case Format::BGRA8Srgb:
        case Format::RG16Float:
        case Format::R32Float:
        case Format::R32Uint:
        case Format::RGB10A2Unorm:
        case Format::RG11B10Float:
        case Format::D24UnormS8Uint:
        case Format::D32Float: return 4;
        case Format::RGBA16Float:
        case Format::RG32Float:
        case Format::RG32Uint: return 8;
        case Format::RGB32Float: return 12;
        case Format::RGBA32Float:
        case Format::RGBA32Uint: return 16;
        default: return 0;
        }
    }
}
