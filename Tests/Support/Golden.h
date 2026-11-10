#pragma once

#include <Helix/Core/Containers/Array.h>
#include <Helix/Core/Strings/Format.h>
#include <Helix/Image/Codecs/Png.h>
#include <Helix/Platform/IO/FileSystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace helix::rhi::testing
{
    // Compares RGBA8 pixels with Tests/Golden/<name>.png, allowing `tolerance` per channel. With HELIX_UPDATE_GOLDEN=1 the
    // first backend that renders a name writes its file; every other backend and GPU is then compared against it.
    // A mismatch writes the rendered image to the temp directory for inspection.
    inline bool MatchGolden(const char* name, const u32* pixels, u32 width, u32 height, u32 tolerance = 2)
    {
        String path = platform::JoinPath(HELIX_RHI_GOLDEN_DIR, helix::Format("{}.png", name));
        image::Image rendered;
        rendered.Allocate(width, height, image::ImageFormat::RGBA8);
        std::memcpy(rendered.pixels.Data(), pixels, rendered.pixels.Size());

        static Array<String> written;
        const char* update = std::getenv("HELIX_UPDATE_GOLDEN");
        bool first = true;
        for (const String& w : written)
            first = first && !(w == StringView(name));
        if (update && update[0] == '1' && first)
        {
            Array<u8> file;
            image::EncodePng(rendered, file, 9);
            written.Push(String(name));
            std::printf("    golden %s written\n", name);
            return platform::WriteFile(path, file);
        }

        Array<u8> file;
        image::Image golden;
        if (!platform::ReadFile(path, file) || !image::DecodePng(file, golden))
        {
            std::printf("    golden %s missing, run with HELIX_UPDATE_GOLDEN=1\n", name);
            return false;
        }
        if (golden.width != width || golden.height != height)
        {
            std::printf("    golden %s is %ux%u, rendered %ux%u\n", name, golden.width, golden.height, width, height);
            return false;
        }
        u32 bad = 0, worst = 0;
        for (usize i = 0; i < golden.pixels.Size(); i++)
        {
            i32 a = golden.pixels[i], b = rendered.pixels[i];
            u32 diff = u32(a > b ? a - b : b - a);
            worst = Max(worst, diff);
            bad += diff > tolerance ? 1 : 0;
        }
        if (!bad)
            return true;
        String actual = platform::JoinPath(platform::TempDirectory(), helix::Format("{}.actual.png", name));
        Array<u8> out;
        image::EncodePng(rendered, out);
        platform::WriteFile(actual, out);
        std::printf("    golden %s: %u channels differ by more than %u (worst %u), rendered image in %s\n", name, bad, tolerance, worst,
                    actual.CStr());
        return false;
    }
}
