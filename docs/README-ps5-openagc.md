# PS5 SDL2 OpenAGC acceleration

## Goal

Add an optional native SDL renderer named `ps5agc` while retaining the PS5
software framebuffer and OSMesa paths as software fallbacks. OpenAGC is the
only authority for firmware, ABI, hardware, mode, and compatibility policy.

## SDL and OpenAGC integration

- Add `SDL_PS5_OPENAGC`, disabled by default. When enabled, CMake requires the
  `OpenAGC` config package and links only `OpenAGC::openagc`.
- Register `ps5agc` ahead of SDL's software renderer. Automatic renderer
  selection may fall back to software; explicitly requesting `ps5agc` reports
  its OpenAGC error.
- Make PS5 presentation lazy and exclusive. The software framebuffer and
  OpenAGC renderer acquire presentation ownership independently, and every
  partial initialization failure releases it.
- Query display resolution and refresh behavior with
  `agcVideoOutGetDefaultMode`; SDL does not contain firmware or hardware
  qualification tables.
- Keep the existing CPU tiled presenter only in the software framebuffer path.
- Use three caller-owned direct VideoOut buffers as the screen render targets.
  Native draws write the current scanout buffer directly, then transition it
  for presentation, signal a bounded EOP fence, and call
  `agcVideoOutPresent`. The accelerated path has no full-frame CPU rasterizer
  and no full-frame presentation copy.
- Check generated PSBC shader blobs into the SDL source tree so consuming SDK
  and application builds do not need a shader compiler.

## Renderer coverage

The renderer advertises accelerated rendering, target textures, and VSYNC. It
implements viewport, clipping, scaling, clears, points, lines, rectangles,
copy/copy-ex, indexed geometry, texture modulation and supported custom blend
modes, render targets, streaming locks, and readback. Commands are batched as
position/UV/color vertices and SDL primitives expand to triangles.

ABGR8888 is the canonical packed texture and target format. IYUV, YV12, NV12,
and NV21 use R8 planes (and RG8 interleaved chroma), with JPEG, BT.601, and
BT.709 conversion shaders. Plane order remains native for YV12 and NV21.

Only OpenAGC-exposed presentation modes are supported. If OpenAGC provides
FIFO/VSYNC only, disabling VSYNC fails instead of being emulated.

## Packaging

- Export the OpenAGC static dependency through installed CMake targets,
  `sdl2.pc`, and `sdl2-config`.
- Publish immutable releases for OpenAGC, Vulkan-PS5, and openagc-psbc.
- Package `ps5-payload-openagc` for Prospero with examples, tests, and PSBC
  packaging disabled and position-independent code enabled.
- Make `ps5-payload-sdl2` depend on the minimum validated OpenAGC tag and pass
  `-DSDL_PS5_OPENAGC=ON`.
- Pin archive checksums and install OpenAGC before SDL2 in `ci-libs.sh`.

## Validation

- Configure and build SDL with OpenAGC both disabled and enabled; validate
  installed consumers through CMake, pkg-config, and `prospero-sdl2-config`.
- Verify automatic selection, explicit selection, initialization failure,
  mode-query failure, allocation failure, presentation failure, and renderer
  recreation without ownership or memory leaks.
- Run SDL geometry, render-target, scaling, sprite, copy-ex, readback, texture
  churn, and YUV visual tests. Cover all YUV matrices, odd rectangles, and
  non-tight pitches.
- Stress triple-buffer reuse and submission failures. GPU and presentation
  waits must be bounded and surfaced as SDL errors.
- Verify stable refresh presentation without CPU tiling threads or full-frame
  CPU copies on the accelerated path.
- Verify software fallback on OpenAGC-rejected hardware and run existing
  OSMesa tests unchanged.

## Later accelerated OpenGL milestone

1. Package Vulkan-Headers, openagc-psbc, and Vulkan-PS5 in dependency order.
2. Keep platform support policy in Vulkan-PS5/OpenAGC and consume only public
   Vulkan results from Mesa and SDL.
3. Extend Vulkan-PS5 until a pinned Mesa/Zink capability probe passes using
   implemented Vulkan features.
4. Enable Zink with swrast and add a PS5 EGL/WSI bridge over the Vulkan-PS5
   VideoOut swapchain.
5. Select Zink for accelerated SDL OpenGL contexts while retaining OSMesa as
   the software option.
6. Do not package `glonagc` until it uses Mesa Gallium interfaces and passes
   upstream hardware validation.

## Assumptions

- OpenAGC exclusively owns firmware, model, ABI, mode, and hardware-validation
  policy.
- The PS5 backend remains single-window and single-display until public
  OpenAGC APIs expose more.
- OSMesa remains software-only; accelerated OpenGL is a Vulkan-PS5/Zink
  milestone.

## Building the current integration

OpenAGC remains opt-in. For a Prospero build, install a tagged OpenAGC package
into the SDK sysroot (or set `OpenAGC_DIR` to its config-package directory),
then configure SDL with:

```sh
prospero-cmake -S . -B build-ps5 \
  -DSDL_PS5_OPENAGC=ON \
  -DOpenAGC_DIR=/path/to/lib/cmake/OpenAGC
cmake --build build-ps5
```

The repository helper accepts the same choice through the
`SDL_PS5_OPENAGC` environment variable. Its default is `OFF`, preserving
generic SDK builds.

The native renderer now consumes SDL's common geometry expansion for fills,
copies, rotated copies, and indexed geometry, packs position/UV/color vertices,
and submits them with `agcGfx1013DrawBaselineIndexAuto`. Clears and points are
also expanded to triangles. Viewports and clip rectangles become OpenAGC
scissors, SDL blend modes become OpenAGC color-target blend state, render-target
textures use flexible GPU-visible memory, and texture locks or updates publish
only texture data. Readback invalidates and converts only the requested
rectangle after a bounded GPU-to-host transition.

The checked-in Wave32 PSBC shaders are built from
`src/render/ps5agc/shaders/ps5agc.vert` and `ps5agc.frag`. Application and SDK
builds consume their generated headers directly and do not invoke a shader
compiler. Native planar YUV formats and their JPEG/BT.601/BT.709 shader variants
remain a later renderer increment; SDL currently converts those formats to the
advertised ABGR8888 texture format.

The Prospero build and link validation covers SDL itself plus `testgeometry`,
`testrendercopyex`, and `testrendertarget`. On firmware 5.50 hardware,
`testgeometry` has also validated native 1920x1080 OpenAGC initialization,
VideoOut registration, shader execution, indexed geometry, and presentation.
The three scanout images occupy one aligned main-direct allocation so their
VideoOut addresses share the required mapping and registration properties.
Linking an OpenAGC consumer requires
the SDK's `libSceAgcDriver` import stub, generated locally from a legally
obtained firmware SPRX; the firmware binary is not distributed by SDL or
OpenAGC. Running and visually validating those programs still requires hardware
accepted by OpenAGC.

Installed static CMake targets discover `OpenAGC::openagc` transitively.
`sdl2.pc` and `sdl2-config --static-libs` also include `openagc`, `kernel`, and
`SceAgcDriver` after SDL's existing platform dependencies.
