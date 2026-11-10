#include <Support/CompiledShaders.h>
#include <Support/TestDevice.h>

#include <Helix/Testing/Test.h>

using namespace helix;
using namespace helix::rhi;
using namespace helix::rhi::testing;

// One 64 x 64 frame mixing color interpolation, depth testing, a repeating nearest sampled texture and alpha blending,
// compared with the same golden image on every backend and GPU.
HX_TEST(Scene, MixedPassMatchesGolden)
{
    ForEachBackend([&](Device& d) {
        ShaderHandle vs = MakeShader(d, kSceneVertex);
        ShaderHandle fs = MakeShader(d, kSceneFragment);
        HX_REQUIRE(vs.IsValid() && fs.IsValid());
        PipelineDesc pd;
        pd.vertexShader = vs;
        pd.fragmentShader = fs;
        pd.vertexBuffers[0] = {36, StepMode::Vertex};
        pd.vertexBufferCount = 1;
        pd.attributes[0] = {0, 0, VertexFormat::Float3, 0};
        pd.attributes[1] = {1, 0, VertexFormat::Float4, 12};
        pd.attributes[2] = {2, 0, VertexFormat::Float2, 28};
        pd.attributeCount = 3;
        pd.depthTest = pd.depthWrite = true;
        pd.depthCompare = CompareFunction::Less;
        pd.depthFormat = Format::D32Float;
        PipelineHandle opaque = d.CreatePipeline(pd);
        pd.depthWrite = false;
        pd.blend[0] = {true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOp::Add};
        PipelineHandle blended = d.CreatePipeline(pd);
        HX_REQUIRE(opaque.IsValid() && blended.IsValid());

        // x, y, z, r, g, b, a, u, v
        const f32 vertices[] = {
            -0.9f, -0.9f, 0.5f, 1, 0, 0, 1, 0, 0,  0.9f,  -0.9f, 0.5f, 0, 1, 0, 1, 0, 0,  0.0f,  0.9f, 0.5f, 0, 0, 1, 1, 0, 0,
            -0.6f, -0.2f, 0.3f, 1, 1, 1, 1, 0, 2,  0.1f,  -0.2f, 0.3f, 1, 1, 1, 1, 2, 2,  -0.6f, 0.5f, 0.3f, 1, 1, 1, 1, 0, 0,
            0.1f,  -0.2f, 0.3f, 1, 1, 1, 1, 2, 2,  0.1f,  0.5f,  0.3f, 1, 1, 1, 1, 2, 0,  -0.6f, 0.5f, 0.3f, 1, 1, 1, 1, 0, 0,
            -0.2f, -0.7f, 0.4f, 1, 1, 0, 0.5f, 0, 0, 0.95f, -0.3f, 0.4f, 1, 1, 0, 0.5f, 0, 0, 0.3f, 0.8f, 0.4f, 1, 1, 0, 0.5f, 0, 0,
        };
        BufferHandle vb = d.CreateBuffer({.size = sizeof(vertices), .usage = BufferVertex, .initialData = vertices});
        u32 checker[16];
        for (u32 i = 0; i < 16; i++)
            checker[i] = ((i % 4) + (i / 4)) % 2 ? 0xff202020u : 0xffe0e0e0u;
        TextureHandle tex = d.CreateTexture({.format = Format::RGBA8Unorm, .width = 4, .height = 4, .initialData = checker});
        SamplerHandle sampler = d.CreateSampler({.minFilter = Filter::Nearest, .magFilter = Filter::Nearest, .mipFilter = Filter::Nearest});

        Target t(&d, true, 64);
        auto mode = [&](CommandList& cl, u32 m) {
            TransientAllocation a = d.AllocateTransient(16);
            const u32 data[4] = {m, 0, 0, 0};
            std::memcpy(a.data, data, sizeof(data));
            cl.SetUniformBuffer(0, a);
        };
        CommandList cl;
        cl.BeginRenderPass(t.Pass(0.05f, 0.05f, 0.2f, 1));
        cl.SetPipeline(opaque);
        cl.SetVertexBuffer(0, vb);
        cl.SetTexture(0, tex, sampler);
        mode(cl, 0);
        cl.Draw(3, 1, 0);
        mode(cl, 1);
        cl.Draw(6, 1, 3);
        cl.SetPipeline(blended);
        mode(cl, 0);
        cl.Draw(3, 1, 9);
        cl.EndRenderPass();
        d.Submit(cl);
        HX_REQUIRE(t.Read());
        HX_CHECK(t.Golden("Scene.MixedPass"));

        d.Destroy(sampler);
        d.Destroy(tex);
        d.Destroy(vb);
        d.Destroy(opaque);
        d.Destroy(blended);
        d.Destroy(vs);
        d.Destroy(fs);
    });
}
