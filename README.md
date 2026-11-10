# Helix.RHI

Rendering hardware interface of the Helix engine: one API over Direct3D 11 and the OpenGL family
(OpenGL 4.6 to 3.3 core, OpenGL ES 3.x, WebGL 2), built on the Helix.D3D11 and Helix.GL bindings.

## Model

- Resources are typed handles (`BufferHandle`, `TextureHandle`, `SamplerHandle`, `ShaderHandle`, `PipelineHandle`) with generation checks.
- A `PipelineDesc` holds shaders, vertex layout, topology, raster, depth and blend state.
- A `CommandList` is a compact byte stream recorded without virtual calls on any thread; `Device::Submit` translates it on the device thread.
- Uniform data per draw comes from `AllocateTransient`, a per-frame ring that is uploaded once per submit (constant buffer offsets on D3D11.1, `glBindBufferRange` on GL).
- A render pass without textures targets the swap chain back buffer.
- Offscreen images have the same memory layout on every backend (top row first): GL uses clip control with an upper left origin when available, `ReadTexture` returns rows top first everywhere.
- Shaders: DXBC bytecode for D3D11, GLSL or ESSL source for GL with a name to slot binding table.

## Layout

| Path | Content |
|---|---|
| `Source/Helix/RHI/Descriptors/` | `Descriptors.h`: handles, formats and resource, pipeline and pass descriptions |
| `Source/Helix/RHI/CommandList/` | `CommandList.h` |
| `Source/Helix/RHI/Device/` | `Device.h`, device creation and backend selection |
| `Source/Helix/RHI/Bindless/` | `Bindless.h`, descriptor table shared by both backends |
| `Source/Helix/RHI/Resources/` | Handle pools |
| `Source/Helix/RHI/Backends/D3D11/`, `Backends/GL/` | Backends, context creation in `Platform/<Os>/` |
| `Tests/` | Device tests, `Tests/Support/` holds shaders and the discrete GPU entry point |
| `Examples/Triangle/` | Rotating triangle |

## Backends

| Backend | Platforms | Context |
|---|---|---|
| D3D11 | Windows | FL 10_0 to 11_1, flip discard swap chain with tearing, WARP fallback |
| OpenGL | Windows, Linux | WGL or EGL, 4.6 down to 3.3 core, hidden window or pbuffer without a window |
| OpenGL ES | Android, Linux | EGL, 3.0 to 3.2 |
| WebGL 2 | Web | Canvas selected by the window handle |

## Example

```cpp
using namespace helix::rhi;
Device* device = Device::Create({.backend = Backend::D3D11, .window = window->GetNativeHandle()});
CommandList cl;
cl.BeginRenderPass({});
cl.SetPipeline(pipeline);
cl.SetVertexBuffer(0, vertices);
cl.SetUniformBuffer(0, device->AllocateTransient(64));
cl.Draw(3);
cl.EndRenderPass();
device->Submit(cl);
device->Present();
```

`Examples/Triangle/Triangle.cpp` renders a rotating triangle with OpenGL, OpenGL ES or WebGL 2.

## Tests

The tests render offscreen on every available backend and check pixels: clear, triangle
orientation, transient uniforms with scissor, texture sampling orientation, instanced indexed
quads, depth testing, swap chain presentation and a 10000 draw benchmark. D3D11 tests compile
their HLSL with the operating system compiler; it is never linked into the library.

## Build

```
git clone --recursive https://github.com/kisstp2006/Helix.RHI
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

## License

GPL-3.0, see `LICENSE`.
