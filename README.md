# Helix.RHI

Rendering hardware interface of the Helix engine: one API over Direct3D 11 and the OpenGL family
(OpenGL 4.6 to 3.3 core, OpenGL ES 3.x, WebGL 2), built on the Helix.D3D11 and Helix.GL bindings.

## Model

- Resources are typed handles (`BufferHandle`, `TextureHandle`, `SamplerHandle`, `ShaderHandle`, `PipelineHandle`) with generation checks.
- A `PipelineDesc` holds shaders, vertex layout, topology, raster, depth and blend state.
- A `CommandList` is a compact byte stream recorded without virtual calls on any thread; `Device::Submit` translates it on the device thread.
- Uniform data per draw comes from `AllocateTransient`, a ring with constant buffer offsets on D3D11.1 and `glBindBufferRange` on GL.
  It is lock free and safe from any thread: a thread takes a chunk of the ring with one compare-and-swap and allocates from it
  privately. A command list remembers the ranges it binds, and `Submit` uploads exactly those, so lists can be recorded in
  parallel and submitted in any order. On GL 4.4 (`ARB_buffer_storage`) the ring is persistently mapped and written in place;
  fences release its memory once the GPU has passed the frame.
- When several threads record, bracket the frame with `BeginFrame` and `EndFrame` and submit its lists in between. One frame
  (or one submit outside frames) may use at most `DeviceDesc::transientBufferSize` bytes.
- A render pass without textures targets the swap chain back buffer.
- Offscreen images have the same memory layout on every backend (top row first): GL uses clip control with an upper left origin when available, `ReadTexture` returns rows top first everywhere.
- Shaders: DXBC bytecode for D3D11, GLSL or ESSL source for GL with a name to slot binding table.

## Compute, storage and indirect

- `CreateComputePipeline` takes a compute shader; `SetPipeline` selects it for `Dispatch` and `DispatchIndirect`.
- `SetStorageBuffer(slot, buffer, access)` binds a whole buffer, `SetStorageTexture(slot, texture, mip)` one mip level as an image.
  Slots 0 to 6 are shared by both; on D3D11 read-write resources use `u<slot>`, read-only buffers `t<16 + slot>`, on GL
  buffers use SSBO binding `slot` and textures image unit `slot`.
- A storage buffer with `structureStride` is a structured buffer on D3D11; combined with vertex, index or indirect use it is a
  raw (`ByteAddressBuffer`) buffer instead.
- `DrawIndirect`, `DrawIndexedIndirect` and `DispatchIndirect` read `DrawIndirectArgs`, `DrawIndexedIndirectArgs` and
  `DispatchIndirectArgs`, which have the same layout on every backend. Several draws use multi draw indirect where available.
- Compute writes are visible to every later command: GL issues one memory barrier before the next command that may read them,
  D3D11 binds UAVs only for the duration of a dispatch.
- Requires D3D11 feature level 11_0, OpenGL 4.3 or OpenGL ES 3.1 (`DeviceCaps::compute`, `drawIndirect`); WebGL 2 has none.

## Bindless buffers

Buffers created with `BufferDesc::bindless` are suballocated from one raw heap per device (`DeviceDesc::bindlessBufferHeapSize`,
64 MB by default, 256 byte aligned) and stay normal buffers for vertex, index, storage, indirect, update, copy and read use.
`RegisterBindlessBuffer` returns an index into a table of byte `(offset, size)` pairs, so shaders reach any buffer without a
binding:

| Backend | Table | Heap |
|---|---|---|
| D3D11 | `StructuredBuffer<uint2>` at `t125` | `ByteAddressBuffer` at `t126`, `RWByteAddressBuffer` at `u7` in compute |
| GL | SSBO binding 8, `uvec2[]` | SSBO binding 9, `uint[]` |

In compute shaders on D3D11, read bindless buffers through the heap UAV rather than a read-only storage binding of the same
buffer. Requires `DeviceCaps::bindlessBuffers` (D3D11 feature level 11_0, GL 4.3 with storage blocks in vertex and fragment
shaders).

## GPU timers

`BeginGpuScope(name)` and `EndGpuScope()` nest; frames end at `EndFrame`. `ReadGpuTimings` returns the scopes of the newest
frame whose timestamps are ready, a few frames later, without stalling. While a Helix.Profiler capture runs, the scopes are also
recorded on the profiler track `GPU`. Available on D3D11, OpenGL 3.3+ and OpenGL ES with `GL_EXT_disjoint_timer_query`
(`DeviceCaps::gpuTimers`).

## Layout

| Path | Content |
|---|---|
| `Source/Helix/RHI/Descriptors/` | `Descriptors.h`: handles, formats and resource, pipeline and pass descriptions |
| `Source/Helix/RHI/CommandList/` | `CommandList.h` |
| `Source/Helix/RHI/Device/` | `Device.h`, device creation and backend selection |
| `Source/Helix/RHI/Bindless/` | `Bindless.h`, descriptor table shared by both backends |
| `Source/Helix/RHI/Resources/` | Handle pools, range allocator of the bindless buffer heap |
| `Source/Helix/RHI/Timing/` | Timestamp query frames shared by both backends |
| `Source/Helix/RHI/Transient/` | Lock free transient ring and upload tracking shared by both backends |
| `Source/Helix/RHI/Backends/D3D11/`, `Backends/GL/` | Backends, context creation in `Platform/<Os>/` |
| `Tests/` | Device tests by topic, `Tests/Support/` holds the test device, shaders and the discrete GPU entry point |
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
quads, depth testing, swap chain presentation, bindless textures and buffers, compute into storage buffers and
textures, a GPU driven indirect draw, GPU timer scopes, transient ring wrapping across submits, command lists recorded on worker threads and a 10000 draw benchmark. D3D11 tests compile
their HLSL with the operating system compiler; it is never linked into the library.

Rendered images are compared with PNG golden images in `Tests/Golden/` (Helix.Image codec, at most 2 per channel apart), so
D3D11, GL, WARP and llvmpipe on every GPU must produce the same picture. After an intended change, regenerate them with
`HELIX_UPDATE_GOLDEN=1`: the first backend that renders a test writes its file and the others are compared against it. A
mismatch leaves `<name>.actual.png` in the temp directory.

## Build

```
git clone --recursive https://github.com/kisstp2006/Helix.RHI
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

## License

GPL-3.0, see `LICENSE`.
