#include <Helix/RHI/Resources/Implementation/RangeAllocator.h>
#include <Helix/Testing/Test.h>

using namespace helix;
using namespace helix::rhi;

HX_TEST(RangeAllocator, AlignsFitsAndMerges)
{
    RangeAllocator a;
    a.Reset(4096);
    u64 x = a.Allocate(100, 256);
    u64 y = a.Allocate(300, 256);
    u64 z = a.Allocate(256, 256);
    HX_CHECK_EQ(x, 0u);
    HX_CHECK_EQ(y, 256u);
    HX_CHECK_EQ(z, 768u);
    HX_CHECK_EQ(a.Allocate(4096, 256), RangeAllocator::kInvalid);
    a.Free(y, 300, 256);
    HX_CHECK_EQ(a.Allocate(512, 256), 256u);
    a.Free(256, 512, 256);
    a.Free(x, 100, 256);
    a.Free(z, 256, 256);
    HX_CHECK_EQ(a.FreeRangeCount(), 1u);
    HX_CHECK_EQ(a.FreeBytes(), 4096u);
    HX_CHECK_EQ(a.Allocate(4096, 256), 0u);
    HX_CHECK_EQ(a.FreeBytes(), 0u);
}
