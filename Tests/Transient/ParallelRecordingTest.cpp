#include <Support/TestDevice.h>

#include <Helix/Jobs/Jobs.h>
#include <Helix/Testing/Test.h>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

namespace
{
    constexpr u32 kTarget = 64;
    constexpr u32 kLists = 8;
    constexpr u32 kDrawsPerList = kTarget * kTarget / kLists;

    u32 PixelColor(u32 pixel, u32 frame) { return 0xff000000u | ((pixel * 7 + frame * 31) & 0xff) | (((pixel >> 4) & 0xff) << 8) | ((frame * 16 & 0xff) << 16); }

    struct RecordContext
    {
        Device* device;
        CommandList* lists;
        RenderPassDesc first, next;
        PipelineHandle pipeline;
        BufferHandle vertices;
        u32 frame;
    };

    // Runs on worker threads: allocates uniform memory and records one list per job.
    void RecordLists(void* user, u32 begin, u32 end)
    {
        const RecordContext& c = *static_cast<const RecordContext*>(user);
        for (u32 list = begin; list < end; list++)
        {
            CommandList& cl = c.lists[list];
            cl.Reset();
            cl.BeginRenderPass(list == 0 ? c.first : c.next);
            cl.SetPipeline(c.pipeline);
            cl.SetVertexBuffer(0, c.vertices);
            for (u32 k = 0; k < kDrawsPerList; k++)
            {
                u32 pixel = list * kDrawsPerList + k;
                u32 color = PixelColor(pixel, c.frame);
                f32 rgba[4] = {f32(color & 0xff) / 255.0f, f32((color >> 8) & 0xff) / 255.0f, f32((color >> 16) & 0xff) / 255.0f, 1.0f};
                TransientAllocation a = c.device->AllocateTransient(sizeof(rgba));
                std::memcpy(a.data, rgba, sizeof(rgba));
                cl.SetViewport({f32(pixel % kTarget), f32(pixel / kTarget), 1, 1, 0, 1});
                cl.SetUniformBuffer(0, a);
                cl.Draw(3);
            }
            cl.EndRenderPass();
        }
    }
}

// Eight command lists per frame are recorded on worker threads with their own uniform data, then submitted in order.
// 4096 draws use 1 MB of a 4 MB ring per frame, so the ring wraps every few frames while older frames are in flight.
HX_TEST(Transient, ParallelRecordingAcrossFrames)
{
    const ShaderPair shaders = {
        "float4 main(float2 p : TEXCOORD0) : SV_Position { return float4(p, 0.5, 1); }",
        "cbuffer C : register(b0) { float4 color; }; float4 main() : SV_Target { return color; }",
        "layout(location = 0) in vec2 p; void main() { gl_Position = vec4(p, 0.5, 1.0); }",
        "layout(std140) uniform C { vec4 color; }; out vec4 o; void main() { o = color; }",
    };
    const ShaderBinding bindings[] = {{"C", BindingKind::UniformBuffer, 0}};
    jobs::Initialize();
    ForEachBackend(
        [&](Device& d) {
            ShaderHandle vs = MakeShader(d, ShaderStage::Vertex, shaders);
            ShaderHandle fs = MakeShader(d, ShaderStage::Fragment, shaders, bindings, 1);
            HX_REQUIRE(vs.IsValid() && fs.IsValid());
            PipelineDesc pd;
            pd.vertexShader = vs;
            pd.fragmentShader = fs;
            pd.vertexBuffers[0] = {8, StepMode::Vertex};
            pd.vertexBufferCount = 1;
            pd.attributes[0] = {0, 0, VertexFormat::Float2, 0};
            pd.attributeCount = 1;
            PipelineHandle pipe = d.CreatePipeline(pd);
            HX_REQUIRE(pipe.IsValid());
            const f32 tri[] = {-1, -1, 3, -1, -1, 3};
            BufferHandle vb = d.CreateBuffer({.size = sizeof(tri), .usage = BufferVertex, .initialData = tri});
            Target t(&d, false, kTarget);

            CommandList lists[kLists];
            RecordContext context{&d, lists, t.Pass(), t.Pass(), pipe, vb, 0};
            context.next.colors[0].load = LoadOp::Load;
            u32 wrong = 0;
            for (u32 frame = 0; frame < 12; frame++)
            {
                context.frame = frame;
                d.BeginFrame();
                jobs::Complete(jobs::ScheduleParallelFor(&RecordLists, &context, kLists, 1));
                for (CommandList& cl : lists)
                    d.Submit(cl);
                d.EndFrame();
                HX_REQUIRE(t.Read());
                u32 before = wrong, firstBad = 0;
                for (u32 pixel = 0; pixel < kTarget * kTarget; pixel++)
                {
                    bool ok = NearColor(t.At(pixel % kTarget, pixel / kTarget), PixelColor(pixel, frame));
                    if (!ok && wrong == before)
                        firstBad = pixel;
                    wrong += ok ? 0 : 1;
                }
                if (wrong != before)
                    std::printf("    frame %u: %u wrong pixels, first %u (list %u, draw %u), got %08x want %08x\n", frame, wrong - before, firstBad,
                                firstBad / kDrawsPerList, firstBad % kDrawsPerList, t.At(firstBad % kTarget, firstBad / kTarget), PixelColor(firstBad, frame));
            }
            HX_CHECK_EQ(wrong, 0u);
            std::printf("    %s %s: %u wrong pixels in 12 frames recorded on %u workers\n", BackendName(d.Caps().backend), d.Caps().renderer, wrong,
                        jobs::WorkerCount());

            d.Destroy(vb);
            d.Destroy(pipe);
            d.Destroy(vs);
            d.Destroy(fs);
        },
        4u << 20);
    jobs::Shutdown();
}
