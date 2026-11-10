#include <Support/CompiledShaders.h>
#include <Support/TestDevice.h>

#include <Helix/Core/Types/Guid.h>
#include <Helix/Platform/IO/FileSystem.h>
#include <Helix/Profiler/Profiler.h>
#include <Helix/Testing/Test.h>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

HX_TEST(GpuTimers, NestedScopesAndProfilerTrack)
{
    ForEachBackend([&](Device& d) {
        if (!d.Caps().gpuTimers)
        {
            std::printf("    %s: no GPU timers, skipped\n", d.Caps().renderer);
            return;
        }
        ShaderHandle vs = MakeShader(d, kColorVertex);
        ShaderHandle fs = MakeShader(d, kColorFragment);
        PipelineHandle pipe = d.CreatePipeline(ColorPipeline(vs, fs));
        HX_REQUIRE(pipe.IsValid());
        const f32 tri[] = {-1, -1, 1, 0, 0, 1, 3, -1, 1, 0, 0, 1, -1, 3, 1, 0, 0, 1};
        BufferHandle vb = d.CreateBuffer({.size = sizeof(tri), .usage = BufferVertex, .initialData = tri});
        Target t(&d);

        profiler::BeginCapture();
        GpuTiming timings[8];
        u32 count = 0;
        for (int frame = 0; frame < 16 && count == 0; frame++)
        {
            d.BeginFrame();
            CommandList cl;
            cl.BeginGpuScope("Frame");
            cl.BeginGpuScope("Clear");
            cl.BeginRenderPass(t.Pass(0, 0, 1, 1));
            cl.EndRenderPass();
            cl.EndGpuScope();
            cl.BeginGpuScope("Draw");
            cl.BeginRenderPass(t.Pass());
            cl.SetPipeline(pipe);
            cl.SetVertexBuffer(0, vb);
            cl.Draw(3, 64);
            cl.EndRenderPass();
            cl.EndGpuScope();
            cl.EndGpuScope();
            d.Submit(cl);
            d.EndFrame();
            d.WaitIdle();
            count = d.ReadGpuTimings(timings, 8);
        }
        profiler::EndCapture();

        HX_REQUIRE(count == 3u);
        HX_CHECK(StringView(timings[0].name) == "Frame" && timings[0].depth == 0);
        HX_CHECK(StringView(timings[1].name) == "Clear" && timings[1].depth == 1);
        HX_CHECK(StringView(timings[2].name) == "Draw" && timings[2].depth == 1);
        HX_CHECK(timings[0].beginMs == 0.0);
        HX_CHECK(timings[2].beginMs >= timings[1].beginMs);
        HX_CHECK(timings[0].durationMs >= timings[2].durationMs && timings[0].durationMs < 1000.0);
        std::printf("    %s: frame %.3f ms, clear %.3f ms, draw %.3f ms\n", d.Caps().renderer, timings[0].durationMs, timings[1].durationMs,
                    timings[2].durationMs);

        String path = platform::JoinPath(platform::TempDirectory(), helix::Format("helix_rhi_{}.hxprof", Guid::Generate().ToString()));
        HX_REQUIRE(profiler::SaveCapture(path));
        profiler::Capture capture;
        HX_REQUIRE(profiler::LoadCapture(path, capture));
        platform::RemoveFile(path);
        const profiler::CaptureThread* gpu = nullptr;
        for (const auto& th : capture.threads)
            if (th.id == (1ull << 63))
                gpu = &th;
        HX_REQUIRE(gpu != nullptr);
        HX_CHECK(gpu->name == "GPU");
        HX_CHECK(gpu->events.Size() >= 6u);
        HX_CHECK(capture.FindMarker("Draw") != nullptr);

        d.Destroy(vb);
        d.Destroy(pipe);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}
