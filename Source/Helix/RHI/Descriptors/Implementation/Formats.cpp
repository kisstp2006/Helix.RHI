#include <Helix/RHI/Descriptors/Descriptors.h>

namespace helix::rhi
{
    bool IsDepthFormat(Format f) { return f == Format::D16Unorm || f == Format::D24UnormS8Uint || f == Format::D32Float; }

    FormatBlockInfo FormatBlock(Format f)
    {
        switch (f)
        {
        case Format::BC1Unorm: case Format::BC4Unorm: case Format::BC4Snorm: case Format::ETC2RGB8: case Format::ETC2RGB8A1: case Format::EACR11:
        case Format::EACR11Snorm: return {4, 4, 8};
        case Format::BC2Unorm: case Format::BC3Unorm: case Format::BC5Unorm: case Format::BC5Snorm: case Format::BC6HUfloat: case Format::BC6HSfloat:
        case Format::BC7Unorm: case Format::ETC2RGBA8: case Format::EACRG11: case Format::EACRG11Snorm: return {4, 4, 16};
        case Format::ASTC4x4: return {4, 4, 16};
        case Format::ASTC5x4: return {5, 4, 16};
        case Format::ASTC5x5: return {5, 5, 16};
        case Format::ASTC6x5: return {6, 5, 16};
        case Format::ASTC6x6: return {6, 6, 16};
        case Format::ASTC8x5: return {8, 5, 16};
        case Format::ASTC8x6: return {8, 6, 16};
        case Format::ASTC8x8: return {8, 8, 16};
        case Format::ASTC10x5: return {10, 5, 16};
        case Format::ASTC10x6: return {10, 6, 16};
        case Format::ASTC10x8: return {10, 8, 16};
        case Format::ASTC10x10: return {10, 10, 16};
        case Format::ASTC12x10: return {12, 10, 16};
        case Format::ASTC12x12: return {12, 12, 16};
        default: return {1, 1, FormatBytesPerPixel(f)};
        }
    }

    bool IsCompressedFormat(Format f) { return FormatBlock(f).width > 1; }

    u64 FormatImageBytes(Format f, u32 width, u32 height)
    {
        FormatBlockInfo b = FormatBlock(f);
        return u64((width + b.width - 1) / b.width) * ((height + b.height - 1) / b.height) * b.bytes;
    }

    u32 FormatRowBytes(Format f, u32 width)
    {
        FormatBlockInfo b = FormatBlock(f);
        return (width + b.width - 1) / b.width * b.bytes;
    }

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
        case Format::R32Sint:
        case Format::RGB10A2Unorm:
        case Format::RG11B10Float:
        case Format::D24UnormS8Uint:
        case Format::D32Float: return 4;
        case Format::RGBA16Float:
        case Format::RG32Float:
        case Format::RG32Uint:
        case Format::RG32Sint: return 8;
        case Format::RGB32Float: return 12;
        case Format::RGBA32Float:
        case Format::RGBA32Uint:
        case Format::RGBA32Sint: return 16;
        default: return 0;
        }
    }
}
