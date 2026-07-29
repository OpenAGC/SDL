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
and NV21 are advertised directly and use R8 planes (and RG8 interleaved
chroma), with separate checked-in JPEG, BT.601, and BT.709 conversion shaders.
YV12 binds its V/U storage in canonical shader order, while the NV21 RG8
descriptor swaps VU to UV without a CPU RGB conversion. Planar streaming
locks retain SDL's contiguous full-texture layout and publish into the aligned
GPU planes when unlocked.

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
also expanded to triangles. A dedicated solid fragment shader handles clears
and untextured primitives without binding a synthetic sampled texture. Viewports
and clip rectangles become OpenAGC scissors, SDL blend modes become OpenAGC
color-target blend state, render-target textures use flexible GPU-visible
memory, and texture locks or updates publish only texture data. Readback
invalidates and converts only the requested rectangle after a bounded
GPU-to-host transition.

The checked-in Wave32 PSBC shaders are built from the GLSL files under
`src/render/ps5agc/shaders/`. Application and SDK builds consume their
generated headers directly and do not invoke a shader compiler. In addition
to the packed ABGR8888 shader, the tree contains planar and interleaved YUV
variants for JPEG, BT.601, and BT.709 conversion.

Linear texture storage uses 256-byte GPU row pitches for ABGR8888, R8, and
RG8 planes while SDL lock buffers retain their tight application-facing
pitches. This is required for gfx1013 linear-image fetches, including odd
widths; allocating only the tight row size can make a texture fetch cross the
mapped allocation and fault the GPU. Render-target descriptors likewise use
the pitch-derived padded surface width while SDL's viewport and scissor retain
the logical texture dimensions. `testyuv` accepts `--hardware` to select its
native-YUV page, `--frames N` for a bounded hardware run, `--bare` to omit the
clear and text overlay, `--display-probe` to require an exact opaque-red
VideoOut readback, `--target-probe` to validate an untextured clear, and
`--target-texture-probe` to validate texture sampling and readback through an
ABGR8888 render target.

The scanout path keeps three flexible-memory GPU render surfaces separate from
the three write-combined direct-memory buffers registered with VideoOut. Before
a flip or display readback, SDL performs a bounded render-target-to-host
transition, invalidates the completed flexible surface, copies it to the
matching registered buffer, and publishes the direct-memory range. Display
readback is then sourced from that actual registered buffer. This follows the
hardware-qualified OpenAGC cube/graphics presentation model and avoids using a
write-combined scanout allocation as a GPU render target. SDL deliberately does
not use `agcGfx1013CopyBuffer` for scanout until OpenAGC qualifies that transfer
across flexible and direct memory on hardware.

The Prospero build and link validation covers SDL itself plus `testgeometry`,
`testrendercopyex`, and `testrendertarget`. On firmware 5.50 hardware,
`testgeometry` has also validated native 1920x1080 OpenAGC initialization,
VideoOut registration, shader execution, indexed geometry, and presentation.
The three scanout images occupy one aligned main-direct allocation so their
VideoOut addresses share the required mapping and registration properties.
Linking an OpenAGC consumer requires
the SDK's `libSceAgcDriver` import stub, generated locally from a legally
obtained firmware SPRX; the firmware binary is not distributed by SDL or
OpenAGC. The YUV shaders and texture paths cross-build with the current
OpenAGC/openagc-psbc revisions. On firmware 5.50, a CPU-seeded 555x333 target
read back `0xff0000ff` after a red clear, `0xffe8e9e7` after sampling the
ABGR8888 test image, and `0xffe8e8e6` after sampling its native YV12/JPEG
conversion. These results validate render-target clear, packed texture color,
native YUV color within expected rounding, and target readback. Direct
VideoOut/display-surface readback was subsequently hardware-qualified with the
same exact-color probe: firmware 5.50 returned `0xff0000ff` from the registered
scanout buffer after the red clear.

`test/run_ps5agc_display_probe.sh` performs that qualification as a bounded
one-frame WebSrv launch. It uses ps5debug-NG to remove and reject stale
`eboot.bin` processes, uploads the required BMP beside the ELF, requires the
exact readback oracle, rejects PID-scoped fatal events and GPU resets, verifies
the SystemService self-exit sequence, and confirms WebSrv remains reachable.
Set `PS5_HOST` and optionally `SDL_PS5AGC_BUILD_DIR` before running it.
The firmware 5.50 qualification completed through `KillApp()` followed by
`All processes exited`, with no app crash, fatal signal, GFX reset, or stale
native-game process. PS5 executables use SDL2main's non-returning SystemService
self-termination path; returning from a raw WebSrv-loaded ELF can otherwise
fall through its entry trampoline and raise a post-test page fault.

Installed static CMake targets discover `OpenAGC::openagc` transitively.
`sdl2.pc` and `sdl2-config --static-libs` also include `openagc`, `kernel`, and
`SceAgcDriver` after SDL's existing platform dependencies.
