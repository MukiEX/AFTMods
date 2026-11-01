# DivaVK

DivaVK now ships only inside `DivaLinux.dva` (see `src/DivaLinux/README.md`); this folder holds
the Vulkan runtime and the WGL bridge it is built from.

DivaGL's renderer running on Vulkan, as a PD Loader plugin.

DivaGL already reimplements nearly all of the arcade game's rendering on modern OpenGL 4.3.
DivaVK keeps that renderer exactly as it is and replaces the layer underneath it: a purpose-built
implementation of the OpenGL subset that DivaGL and the game actually use, written directly
on Vulkan. No real OpenGL context is ever created, so the plugin can run anywhere a Vulkan
driver exists, including Wine/Proton on Android through Turnip.

DivaGL and its shaders are by korenkonder; DivaVK builds on them and must credit them the same way.

## Layout

| Path | What |
| --- | --- |
| `core/` | Platform-independent GL-on-Vulkan runtime (`divavk_core` static library) |
| `core/include/divavk/divavk.h` | Public API: create, `dvk_get_proc_address`, swap, resize |
| `plugin/vk_bridge.cpp` | Windows/Wine bridge: WGL emulation and import/table patching |
| `../DivaLinux/` | Builds `DivaLinux.dva`: DivaGL's sources plus the bridge (`DIVAVK` defined) and the FFmpeg movie player |
| `tests/` | Headless test runner (any Vulkan driver, including SwiftShader) |
| `tools/` | Generators for the GL name list and the game's GL slot table |
| `external/` | Vendored: volk, Vulkan Memory Allocator, Khronos GL headers |

## How it fits together

```
diva.exe / GLUT / DivaGL ──GL calls──▶ game GL tables + imports ──patched──▶ DivaVK core ──▶ Vulkan
                                           (plugin/vk_bridge.cpp)
```

- The bridge rewrites the `opengl32.dll`/`gdi32.dll` imports of `diva.exe`, `glut32.dll` and the
  plugin, and writes DivaVK into every slot of the game's GL function tables (addresses come
  from DivaGL's `wrap.cpp`, see `tools/gen_game_tables.py`). It does this at load time and again
  after the game loads its extension pointers.
- WGL is emulated. The Vulkan context is created on the first `wglMakeCurrent`, with a
  `VK_KHR_win32_surface` swapchain on the GLUT window; `SwapBuffers` presents.
- Every GL entry point DivaVK doesn't implement yet resolves to a stub that logs its name once
  to `plugins/DivaLinux.log`. A play session therefore produces the exact to-do list.

## Core design

**Coordinate conventions.** Images keep GL's memory layout (row 0 = bottom). With a normal
positive-height viewport Vulkan produces exactly that layout, so uploads, `glReadPixels`, blits,
texture coordinates and `gl_FragCoord` all match GL with no flipping. The two differences are
handled explicitly: GL's counter-clockwise front face maps to `VK_FRONT_FACE_CLOCKWISE`, and the
final present flips vertically. Clip-space depth is converted from GL's [-w, w] to Vulkan's
[0, w] by a vertex-shader epilogue, so depth buffer contents are identical to GL's.

**Shaders.** DivaGL passes GL-style GLSL (it strips its own `set = N` qualifiers). DivaVK compiles
it with glslang in "Vulkan rules relaxed" mode and separates the binding namespaces into one
descriptor set: texture unit N → binding N, UBO binding N → 32+N, SSBO binding N → 48+N.
`glGetProgramBinary`/`glProgramBinary` store SPIR-V, so DivaGL's program cache skips glslang on
later runs.

**Render passes.** Inferred from GL usage: a pass starts at the first draw/clear on a
framebuffer and ends when something needs the GPU outside it. A `glClear` before the pass begins
becomes the pass's load op (what tile-based GPUs want); a mid-pass clear uses
`vkCmdClearAttachments`. Every image rests in a fixed layout between operations
(shader-read-only for sampled images), so draws never need mid-pass barriers.

**Buffers.** Small/dynamic buffers keep a CPU shadow; each write after use creates a new
per-frame version in an upload arena. This gives GL's ordering (each draw sees the data current
when it was issued) for DivaGL's heavy per-draw UBO updates without stalls. Static buffers live in
device memory and are updated with staged copies.

**Uploads.** Copies into resources not yet used in the current frame go into a separate
command buffer submitted ahead of the frame, so they never break a render pass. Only updates to
something already used this frame are recorded inline.

## Building

### Core library and tests (Linux)

```sh
cmake -S src/DivaVK -B build -G Ninja \
  -DDIVAVK_VULKAN_HEADERS_DIR=/path/to/Vulkan-Headers \
  -DDIVAVK_GLSLANG_DIR=/path/to/glslang
ninja -C build
VK_ICD_FILENAMES=/path/to/vk_swiftshader_icd.json ./build/divavk_tests   # or any real driver
./build/divavk_tests --validate   # with VK_LAYER_KHRONOS_validation installed
```

### PD Loader plugin (Windows, Visual Studio 2019+)

1. Check out [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) into
   `src/DivaVK/external/Vulkan-Headers` (or set the `DivaVKVulkanHeaders` MSBuild property) and
   [glslang](https://github.com/KhronosGroup/glslang) anywhere.
2. Build the core: `cmake -S src/DivaVK -B src/DivaVK/build -A x64 -DDIVAVK_VULKAN_HEADERS_DIR=... -DDIVAVK_GLSLANG_DIR=...`
   then `cmake --build src/DivaVK/build --config Release`. This stages `divavk_core.lib`,
   `glslang.lib` and `glslang-default-resource-limits.lib` into `src/DivaVK/lib/Release`.
3. Build the `DivaLinux` project in `AFTMods.sln` (it also needs FFmpeg headers, see
   `src/DivaLinux/README.md`). Output: `bin/DivaLinux.dva`.
4. Install it in `plugins/` *instead of* `DivaGL.dva` (they replace the same renderer).
   Settings go in `plugins/DivaLinux.ini` (same keys as `DivaGL.ini`, plus `[movie]`). Set the environment variable
   `DIVAVK_VALIDATION=1` to enable the Khronos validation layer.

## Status (phase 1)

Working and covered by `tests/` (60 checks on SwiftShader, clean under the Khronos validation layer including synchronization validation):

- Buffers (all DivaGL patterns incl. persistent dynamic storage, mapping, PBO readback)
- 2D and cube textures, mip chains defined level by level, `glGenerateMipmap`, swizzles, the
  legacy luminance/alpha/intensity formats, RGB8, half/float formats, BC1–BC5 (BC1 tested)
- Sampler objects and per-texture sampler state, anisotropy, depth compare
- Framebuffers, MRT, renderbuffers, MSAA + resolve, `glBlitFramebuffer` (flips, scaling),
  `glCopyTexSubImage2D`, `glCopyImageSubData`, `glReadPixels`, `glGetTexImage`
- Sampling the depth buffer that is still attached (DivaGL's TAA camera blur, NPR contours):
  the pass switches to a read-only depth layout for those draws
- All fixed-function state DivaGL uses (blend, depth, stencil, cull, scissor, masks, restart)
- GLSL → SPIR-V for every DivaGL shader: all 142 programs (two permutation sets) link and get a pipeline; program binaries
- Occlusion queries (`GL_SAMPLES_PASSED`, any-samples), debug groups as Vulkan labels
- Win32 swapchain presentation (tested here with a headless surface)

Not done yet:

- **Not yet run against the game.** The bridge is written but untested on a real PD Loader
  install; expect a few iterations driven by `DivaVK.log`.
- BC decoding fallback for GPUs without BC support (Mali, some Adreno drivers)
- Legacy immediate mode (`glBegin`…) and client-side vertex arrays (logged if used)
- Partial-color-mask clears, `glVertexAttribDivisor` > 1, non-all-ones primitive restart
  indices (all logged)
- Mobile tuning: load/store `DONT_CARE` where safe, fewer full-screen copies, half-precision

## Roadmap

- **Phase 2:** run the game on desktop Windows, fill whatever `DivaVK.log` reports, BC
  decompression fallback, RenderDoc pass review.
- **Phase 3:** Android: Wine + Box64/FEX + Turnip container, mobile render-pass tuning.
