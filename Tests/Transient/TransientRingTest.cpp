#include <Support/TestDevice.h>

#include <Helix/Testing/Test.h>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

// 256 draws with their own uniform color in 8 submits through a 16 KB ring: the ring wraps four times, so every reuse
// of a region must wait for the GPU to finish the submit that read it.
HX_TEST(Transient, RingWrapsAcrossSubmits)
{
    const ShaderPair shaders = {
        "float4 main(float2 p : TEXCOORD0) : SV_Position { return float4(p, 0.5, 1); }",
        "cbuffer C : register(b0) { float4 color; }; float4 main() : SV_Target { return color; }",
        "layout(location = 0) in vec2 p; void main() { gl_Position = vec4(p, 0.5, 1.0); }",
        "layout(std140) uniform C { vec4 color; }; out vec4 o; void main() { o = color; }",
    };
    const ShaderBinding bindings[] = {{"C", BindingKind::UniformBuffer, 0}};
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
            Target t(&d);

            auto color = [](u32 i) { return 0xff000000u | (i * 37 % 256) | ((i * 11 % 256) << 8) | ((255 - i) << 16); };
            for (u32 batch = 0; batch < 8; batch++)
            {
                CommandList cl;
                if (batch == 0)
                    cl.BeginRenderPass(t.Pass());
                else
                {
                    RenderPassDesc rp = t.Pass();
                    rp.colors[0].load = LoadOp::Load;
                    cl.BeginRenderPass(rp);
                }
                cl.SetPipeline(pipe);
                cl.SetVertexBuffer(0, vb);
                for (u32 k = 0; k < 32; k++)
                {
                    u32 i = batch * 32 + k;
                    u32 c = color(i);
                    f32 rgba[4] = {f32(c & 0xff) / 255.0f, f32((c >> 8) & 0xff) / 255.0f, f32((c >> 16) & 0xff) / 255.0f, 1.0f};
                    TransientAllocation a = d.AllocateTransient(sizeof(rgba));
                    HX_REQUIRE(a.data != nullptr);
                    std::memcpy(a.data, rgba, sizeof(rgba));
                    cl.SetViewport({f32(i % kSize), f32(i / kSize), 1, 1, 0, 1});
                    cl.SetUniformBuffer(0, a);
                    cl.Draw(3);
                }
                cl.EndRenderPass();
                d.Submit(cl);
            }
            HX_REQUIRE(t.Read());
            u32 bad = 0;
            for (u32 i = 0; i < kSize * kSize; i++)
                bad += NearColor(t.At(i % kSize, i / kSize), color(i)) ? 0 : 1;
            HX_CHECK_EQ(bad, 0u);
            std::printf("    %s %s: %u of 256 uniform draws wrong after 4 ring wraps\n", BackendName(d.Caps().backend), d.Caps().renderer, bad);

            d.Destroy(vb);
            d.Destroy(pipe);
            d.Destroy(vs);
            d.Destroy(fs);
        },
        16u << 10);
}
