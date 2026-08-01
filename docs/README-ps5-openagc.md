# PS5 SDL2 OpenAGC acceleration

## Goal

Add an optional native SDL renderer named `ps5agc` while retaining the PS5
software framebuffer and OSMesa paths as software fallbacks. OpenAGC is the
only authority for firmware, ABI, hardware, mode, and compatibility policy.

## SDL and OpenAGC integration

- Add `SDL_PS5_OPENAGC`, disabled by default. When enabled, CMake requires the
  `OpenAGC` config package and links only `OpenAGC::openagc`.
- Keep deterministic failure injection behind
  `SDL_PS5_OPENAGC_TEST_HOOKS`, which is disabled by default, depends on the
  OpenAGC renderer, and is intended only for qualification builds.
- Register `ps5agc` ahead of SDL's software renderer. Automatic renderer
  selection may fall back to software; explicitly requesting `ps5agc` reports
  its OpenAGC error.
- Make PS5 presentation lazy and exclusive. The software framebuffer and
  OpenAGC renderer acquire presentation ownership independently, and every
  partial initialization failure releases it.
- Query display resolution and refresh behavior with
  `agcVideoOutGetDefaultMode`; SDL does not contain firmware or hardware
  qualification tables. While `ps5agc` owns presentation, its reported mode
  is also SDL's public current and desktop display mode; destroying the
  renderer restores the software backend's prior mode.
- Keep the existing CPU tiled presenter only in the software framebuffer path.
- Use three flexible-memory render targets and three caller-owned direct
  VideoOut buffers. Native draws write the current render target, then a
  transitioned `agcGfx1013CopyBuffer` transfer populates the matching scanout
  buffer before a bounded EOP fence and `agcVideoOutPresent`. The accelerated
  path has no full-frame CPU rasterizer or full-frame CPU presentation copy.
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

## Native OpenAGC runtime migration gate

OpenAGC now also publishes a firmware-neutral native runtime
(`openagc/runtime.h`; API version 25 at the reviewed OpenAGC head) with typed
resource transitions, command batches, bounded fences, recycling, capture,
and optional validation diagnostics. It is a future consolidation option, not
yet a replacement for the qualified `ps5agc` presentation path.

- Keep the direct renderer as the qualified path. It obtains the mode through
  `agcVideoOutGetDefaultMode`, owns three caller-provided VideoOut buffers,
  and its direct graphics, copy, fence, and VideoOut APIs remain public in the
  reviewed OpenAGC revision.
- Do not select the runtime from `OpenAGC_VERSION` alone. The project remains
  version 0.2.0, so pin an immutable release that explicitly exports native
  runtime API 25 or later and matching shader contracts (reflection format 2
  and compiler API 15 at the reviewed head). Keep checked-in SDL shader blobs
  and reflection records reproducible from that pinned toolchain.
- Prototype a runtime-backed renderer only after the direct renderer passes its
  complete hardware suite. Device selection, queue ownership, transitions,
  submission, fence waits, and recycling must use public runtime APIs; map
  errors to SDL without exposing OpenAGC internals or firmware policy.
- Do not migrate presentation yet. The current runtime present chain accepts
  only two to sixteen distinct 1920x1080 linear `RGBA8_UNORM` scanout images.
  Its documented combined two-buffer registration/transition/flip experiment
  did not return and remains invalid; never rerun that historical experiment.
- Permit runtime presentation only after it supports SDL's required default
  VideoOut modes, an equivalent of triple-buffer ownership, and a new safe,
  repeated render/copy/present/readback/recreate artifact with finite waits and
  clean teardown on OpenAGC-accepted hardware. This is a new qualification
  run, not an inference from the earlier registration-only ladder.
- Before runtime mode ships, run direct and runtime build/install consumers and
  the target, YUV, readback, selection, fallback, and recreation stress suites.
  Retain the direct renderer and software fallback until that evidence exists.

## Accelerated OpenGL / Zink status

1. Package Vulkan-Headers, OpenAGC, openagc-psbc, Vulkan-PS5, the pinned
   Mesa-Zink runtime, and SDL in dependency order.
2. Keep platform support policy in Vulkan-PS5/OpenAGC and consume only public
   Vulkan results from Mesa and SDL. Keep Vulkan-PS5's direct OpenAGC
   integration as its qualified baseline while the runtime reaches this gate.
3. Vulkan-PS5 now uses the native runtime and its strict pinned capability
   probe passes. Keep the frozen capability profile as a package gate.
4. The pinned Mesa revision can be cross-built as an EGL plus Zink runtime
   with `build-scripts/build-ps5-zink.sh`. This runtime loads
   `libvulkan_ps5.so` and does not replace the independent Mesa 22.1.7 OSMesa
   fallback.
5. SDL's opt-in `SDL_PS5_ZINK` path now creates a native-window EGL surface.
   Mesa's Prospero surfaceless loader lowers the stable SDL window identity to
   `VK_EXT_headless_surface`; Vulkan-PS5 remains the sole owner of swapchain,
   VideoOut, synchronization, and the OpenAGC present chain. Select it
   explicitly with `SDL_HINT_PS5_OPENGL_DRIVER=zink`; missing or malformed
   selection fails closed.
6. The native-window bridge is FW 5.500.008-qualified. Two immediate guarded
   runs (`20260801T032341Z` and `20260801T032359Z`) created the real Zink EGL
   context, returned exact RGBA `64,128,191,255`, presented, released every
   OpenAGC child, self-exited, and relaunched without reboot. The Vulkan-PS5
   generic suite passes 46/46 and its shared-ICD verifier reports 204 exports
   with only supported dynamic relocations. The tested hashes are:

   - `testps5zink`: `95da10acf89da3e35865890874034b8bffef1c563417309a0e4bb98404540ad9`
   - `libvulkan_ps5.so`: `f2f2a176d0dc4dcb14942471af41d9fa6eebdeaba134084a52de280932b71f52`
   - `libEGL.so.1.0.0`: `0d2922b30b3dbbe25f060331043bb4a4732272d0813023568381306528913fc1`
   - `libgallium-26.3.0-devel.so`: `9da905ef314e3362631406b1d85e013071b7fc80661cb663c7c40920d23eef85`

   OSMesa remains the conservative default and independent software fallback;
   FW 11.60 replay is deferred until that endpoint is available.
7. Do not package `glonagc` until it uses Mesa Gallium interfaces and passes
   upstream hardware validation.

## Assumptions

- OpenAGC exclusively owns firmware, model, ABI, mode, and hardware-validation
  policy.
- The PS5 backend remains single-window and single-display until public
  OpenAGC APIs expose more.
- OSMesa remains software-only; accelerated OpenGL is a Vulkan-PS5/Zink
  milestone.
- Mesa-Zink and openagc-psbc intentionally use different pinned Mesa compiler
  snapshots. They communicate through Vulkan and SPIR-V and must not link to
  one another's private Mesa interfaces.

## Building the current integration

OpenAGC remains opt-in. The direct integration requires OpenAGC 0.2.0 or
newer; a future runtime integration must use the separately pinned runtime API
contract above, rather than treating project version 0.2.0 as sufficient. For
a Prospero build, install a tagged OpenAGC package
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
generic SDK builds. An explicit `SDL_HINT_RENDER_DRIVER=ps5agc` request is
strict even in those builds: when the renderer was not compiled, creation
fails with `ps5agc renderer is not available` instead of silently selecting
software. Automatic creation can still select software normally. The
dummy-video `testps5agcselection` regression verifies both outcomes and proves
that the failed explicit request does not retain renderer ownership of the
window.

The native renderer now consumes SDL's common geometry expansion for fills,
copies, rotated copies, and indexed geometry, packs position/UV/color vertices,
builds sequential 32-bit indices in the upload arena, and submits them with
`agcGfx1013DrawBaselineIndexed`. Clears and points are
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

Maintainers regenerate every blob with
`src/render/ps5agc/shaders/regenerate.sh`. The script requires
`glslangValidator`, `xxd`, and the sibling `openagc-psbc/psbc` executable. It
compiles the vertex and pass-through geometry stages together as Wave32 NGG,
then emits both `.sb` binaries and static C headers. Renderer initialization
rejects code offsets at or beyond the blob end, code payloads shorter than 16
bytes, and any fused Wave32 VS/PS state rejected by
`agcGfx1013ValidateWave32VsPs`. These checks caught an older 348-byte NGG-back
blob whose code offset was 344 and therefore contained only four code bytes.

Linear texture storage uses 256-byte GPU row pitches for ABGR8888, R8, and
RG8 planes while SDL lock buffers retain their tight application-facing
pitches. This is required for gfx1013 linear-image fetches, including odd
widths; allocating only the tight row size can make a texture fetch cross the
mapped allocation and fault the GPU. OpenAGC's linear image descriptor has no
explicit pitch field, so sampled descriptors retain the logical width while
storage follows that implicit 256-byte row layout. Render-target descriptors use
the pitch-derived padded surface width while SDL's viewport and scissor retain
the logical texture dimensions. `testyuv` accepts `--hardware` to select its
native-YUV page, `--frames N` for a bounded hardware run, `--bare` to omit the
clear and text overlay, `--display-probe` to require an exact opaque-red
VideoOut readback, `--target-probe` to validate an untextured clear, and
`--target-texture-probe` to validate texture sampling and readback through an
ABGR8888 render target. `--packed-texture-probe` performs the equivalent
packed sampling check directly on the BGRA8 display surface. `--blend-probe`
verifies that a zero-alpha sampled draw preserves an opaque magenta display
surface exactly.

The scanout path keeps three disjoint flexible-memory GPU render surfaces
separate from the three write-combined direct-memory buffers registered with
VideoOut. The shaders and three slot-local groups of render surface, command
buffer, and fence are suballocated from one mode-sized flexible-memory mapping,
with every render target starting on a 64 KiB boundary. The validated 1080p
layout retains a 25 MiB minimum mapping; larger OpenAGC default modes grow the
mapping from their reported dimensions instead of being rejected by an SDL
resolution ceiling. Whether that larger allocation is supported remains an
OpenAGC decision, and its public allocation error flows through normal renderer
fallback. Each slot has its own 256 KiB DCB and monotonically increasing fence
value, so recording a later frame cannot overwrite an in-flight slot. The
frequently rewritten 4 MiB vertex/index upload arena uses public direct
write-combined memory so its
cache traffic stays outside the command/render mapping. This follows OpenAGC's
hardware-qualified cube layout and avoids exhausting the FW 5.50 per-process
system-flexible mapping capacity before the first 8,294,400-byte 1080p target.
Consequently, a 3840x2160 default mode is no longer rejected by a hard-coded
SDL pool limit, but 4K remains unqualified until OpenAGC exposes it on accepted
hardware and the guarded renderer suite passes at that mode.
Every guarded `testyuv` launch using `ps5agc` now requires its renderer output,
current display mode, and desktop display mode to agree, including the integer
refresh rate derived from OpenAGC's millihertz value.
Before a flip or display readback, SDL transitions the completed surface to copy
source and the matching registered buffer to copy destination, records
`agcGfx1013CopyBuffer`, transitions the destination for host readback, and
waits on the bounded EOP fence. Display readback is sourced from that actual
registered buffer. This avoids using a write-combined scanout allocation as a
GPU render target while also avoiding a full-frame CPU copy.

Renderer destruction closes VideoOut, calls the public `agcDriverShutdown`
lifecycle boundary, and then releases SDL-owned GPU-visible allocations. This
order is also used on partial renderer creation failure, before presentation
ownership is returned to the software path. OpenAGC 0.2.0 pairs its
system-flexible allocations with
`sceKernelReleaseFlexibleMemory`; repeating an allocation-failure probe on
firmware 5.50 reused the same addresses for all nine internal regions, showing
that the failed launch no longer consumes additional quota. Flexible memory
leaked by pre-0.2.0 processes remains allocated until the console is restarted.

The Prospero build and link validation covers SDL itself plus `testgeometry`,
`testrendertarget`, `testscale`, `testsprite2`, and `testrendercopyex`. These five
interactive renderer tests accept the common `--frames N` option and log
`Frame limit reached: N` before exiting normally. This makes unattended WebSrv
runs bounded instead of relying on an input event or an external process kill;
zero, negative, malformed, and overflowing limits are rejected. On firmware
5.50 hardware,
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
native YUV color within expected rounding, and target readback. Screen readback
now transitions and reads the current flexible render target before the
separate scanout copy. The display probe queries the actual renderer output
extent and samples its center; using the 320x240 source-image center had sampled
outside the rendered area while OpenAGC's legacy viewport forced a centered
square. It checks every requested frame instead of only frame zero. A
three-frame firmware 5.50 run returned exact `0xff0000ff` on all three render
slots with bounded fences, copies, presentations, and a clean process exit.

`test/run_ps5agc_display_probe.sh` performs that qualification as a bounded
WebSrv launch. It uses read-only ps5debug-NG process-list queries to reject
stale `eboot.elf` and `eboot.bin` processes, uploads the required BMP beside the ELF, requires the
exact readback oracle, rejects PID-scoped fatal events and GPU resets, verifies
the SystemService self-exit sequence, rejects reboot or shutdown sequences, and
confirms WebSrv remains reachable. It never debugger-attaches to or kills a
target process. On every result path it waits up to ten seconds for the
SystemService lifecycle to remove the exact process; a process that remains is
reported as stale and prevents the next launch.
Fatal/reset and power-event checks run before any pixel or test-result oracle,
so a test can never be reported as an ordinary color mismatch after its GPU
submission has already destabilized the shell.
Set `PS5_HOST` and optionally `SDL_PS5AGC_BUILD_DIR` before running it. The
runner rebuilds its selected automation, YUV, standalone, or failure target
before upload, preventing a stale ELF from invalidating a hardware diagnosis.
Set `SDL_PS5AGC_SKIP_BUILD=1` only when intentionally testing an already-built ELF;
`SDL_PS5AGC_BUILD_JOBS` controls build parallelism. Set
`SDL_PS5AGC_PROBE_FRAMES` to a positive value for triple-buffer stress; the
default remains one frame. `SDL_PS5AGC_PROBE_RENDERER=auto` omits the renderer
hint so default selection can be checked, with `SDL_PS5AGC_EXPECT_RENDERER`
naming the expected result. `SDL_PS5AGC_PROBE_ACCELERATED=1` requests the
accelerated capability without naming a driver. Expected creation failures can
be qualified without weakening lifecycle checks by setting
`SDL_PS5AGC_EXPECT_FAILURE=1` and the required fixed-string
`SDL_PS5AGC_EXPECT_ERROR` oracle.

Set `SDL_PS5AGC_PROBE_KIND=recreate` to run the opt-in renderer lifecycle
probe. `SDL_PS5AGC_RECREATE_COUNT` defaults to eight destroy/create cycles in
one process. Every new instance must reacquire OpenAGC and VideoOut, select the
requested renderer, preserve its advertised VSYNC state after rejecting
no-VSYNC, and then release ownership cleanly. The final instance must still
return exact red from the display readback and present successfully. This is a
bounded qualification of repeated driver shutdown/reinitialization and GPU
allocation reuse; it is not run by default.

Set `SDL_PS5AGC_PROBE_KIND=churn` for the bounded texture-lifetime stress
probe. `SDL_PS5AGC_TEXTURE_CHURN_COUNT` defaults to 32 and is capped at 1,000.
Each iteration allocates one streaming ABGR8888 texture and one target texture,
fills the streaming texture through its reported lock pitch, applies a unique
color modulation, and requires matching target and display readbacks within
one unit per channel. Both textures are then destroyed before the next
iteration. The final renderer instance must still return exact red and present
cleanly through the normal guarded lifecycle.

`test/run_ps5agc_selection_matrix.sh` runs four isolated supported-hardware
selection cases through the same guarded launcher: explicit `ps5agc`, automatic
selection, an unnamed accelerated request, and explicit software. If
`SDL_PS5AGC_DISABLED_ELF` names a separately built PS5 `testyuv` with
`SDL_PS5_OPENAGC=OFF`, the matrix also requires automatic software selection
and strict failure of an explicit `ps5agc` request. Those optional cases prove
the backend-absent build contract; they do not replace qualification on
hardware that OpenAGC itself rejects.

`test/run_ps5agc_failure_matrix.sh` runs five deterministic qualification
cases: default-mode query, driver initialization, renderer-pool allocation,
GPU submission, and VideoOut presentation. Configure its build with
`-DSDL_PS5_OPENAGC_TEST_HOOKS=ON`; the guarded launcher refuses to run this
matrix against a cache without that explicit opt-in. The first three cases
require an explicit named request and an unnamed accelerated request to return
the injected upstream-style error, then require ordinary automatic creation to
fall back to software and present successfully on the same window. Submission
and presentation failures must make the accelerated renderer fail-stop; after
destroying it, the software renderer must reacquire presentation and complete
a frame. Each case is a separate WebSrv application and retains the PID-scoped
fatal/reset, power-event, exact-process teardown, kernel-warning, and WebSrv
health gates. The hooks add no firmware, model, ABI, or support-policy logic and
are omitted from normal and pacbrew builds.

The failure-injection ELF cross-builds as a PS5 x86-64 PIE with the current
OpenAGC package. Hardware execution of this new matrix still requires an
explicitly available console.

`test/run_ps5agc_stress_matrix.sh` is the bounded soak entry point. Its default
phases are 180 explicitly accelerated display frames, 128 complete streaming
and target-texture churn cycles, 16 renderer destruction/recreation cycles,
and 30 frames in each of the five standalone renderer programs after the
automated Render suite. Each phase is a separate guarded WebSrv launch (and
each standalone program is isolated again), so stale ownership or teardown
failure stops the matrix before another application starts. Override the
positive limits with `SDL_PS5AGC_STRESS_FRAMES`,
`SDL_PS5AGC_STRESS_CHURN` (capped at 1,000),
`SDL_PS5AGC_STRESS_RECREATE`, and `SDL_PS5AGC_STRESS_SUITE_FRAMES`.
The final PASS line is emitted only after every pixel, renderer, bounded-fence,
fatal/reset, power, lifecycle, kernel-warning, and WebSrv-health oracle has
passed.

The local backend-absent artifact can be reproduced with:

```sh
cmake -S . -B build-ps5-no-openagc \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/prospero.cmake \
  -DCMAKE_CXX_COMPILER_WORKS=1 \
  -DSDL_PS5_OPENAGC=OFF -DSDL_SHARED=OFF -DSDL_STATIC=ON \
  -DSDL_TEST=ON -DSDL_TESTS=ON
cmake --build build-ps5-no-openagc --target testyuv -j4
PS5_HOST=10.0.1.41 \
SDL_PS5AGC_DISABLED_ELF=build-ps5-no-openagc/test/testyuv \
  test/run_ps5agc_selection_matrix.sh
```

`CMAKE_CXX_COMPILER_WORKS=1` only bypasses the SDK's unrelated C++ runtime
link probe; SDL and this test executable are built as C. The current local ELF
is `build-ps5-no-openagc/test/testyuv`; its CMake cache must report
`SDL_PS5_OPENAGC:BOOL=OFF` and `SDL_TESTS:BOOL=ON`. Its hash intentionally is
not pinned because every SDL source commit rebuilds a different test ELF.

`test/run_ps5agc_render_suite.sh` first runs SDL's automated `Render` suite,
then launches `testgeometry`, `testrendertarget`, `testscale`, `testsprite2`,
and `testrendercopyex` as five separate bounded applications. The renderer is
explicitly pinned to `ps5agc`. The automation harness propagates its
`--renderer` argument to suite-created renderers and logs their actual driver;
the standalone applications use `--info render`, and their oracle requires
`ps5agc` under `Current renderer`. A pass therefore cannot silently come from
the software fallback. The runner requires all four enabled automation tests
to pass and each standalone application to reach its exact frame limit. Every
launch independently rejects fatal, reset, and power events and applies the
same exact-process lifecycle checks as the display probe. Set
`SDL_PS5AGC_AUTOMATION_FILTER` to one `render_test*` name to isolate a failing
automation case. `SDL_PS5AGC_SUITE_FRAMES` controls each standalone run and
defaults to three frames, exercising every presentation slot once;
`SDL_PS5AGC_SUITE_STANDALONE_TARGETS` can select a space-separated subset of
the five allowlisted programs.

The standalone renderer programs can also be run locally with `--frames N`, or
individually through the guarded launcher by setting
`SDL_PS5AGC_PROBE_KIND=standalone` and `SDL_PS5AGC_STANDALONE_TARGET`.
Host software-renderer smoke tests complete two bounded frames for all five
programs, including textured indexed geometry in `testgeometry` and
`testsprite2`; the same sources cross-build as PS5 PIE executables with
OpenAGC enabled. Hardware launches remain gated by the guarded WebSrv runner
and an explicitly available console.

Three narrower `testyuv` gates separate display sampling, blending, and render
targets without running the full Render suite. Set
`SDL_PS5AGC_PROBE_KIND=packed` to draw one full-display ABGR8888 texture with
blending disabled and compare its center against a CPU conversion. Set
`SDL_PS5AGC_PROBE_KIND=blend` to draw a zero-alpha ABGR8888 texture over an
opaque magenta display surface and require the destination to remain exactly
`0xffff00ff`. Set `SDL_PS5AGC_PROBE_KIND=target` for the same packed sampling
oracle through an ABGR8888 render target. All three modes use the same bounded
fence, fatal-event, self-exit, stale-process, and WebSrv-health checks as the
display probe.

### Current hardware status

The renderer is still experimental and must not yet be used as the default in
shipping applications. Two upstream OpenAGC defects found through SDL hardware
qualification were fixed on 2026-07-29. The first was an invalid buffer-copy
packet that produced a PFP bad opcode, low-VA write, stuck graphics queue, and
GFX reset; OpenAGC commit `c569d73` replaces it with the validated seven-dword
gfx1013 `DMA_DATA` stream. The second was the legacy viewport helper's use of
`min(width,height)` for X scale, which forced 1920x1080 rendering into a
centered 1080x1080 square with 420-pixel side bars. OpenAGC commit `db91d2a`
maps NDC across the complete requested width and height.

With both fixes linked, firmware 5.50 passes the isolated
`render_testPrimitives` test: all 30 assertions, including exact zero-tolerance
pixel comparison, pass with `ps5agc` explicitly selected. The guarded run
exited through SystemService with no fatal event, GPU reset, power event, or
stale process. A red-clear probe also returns exact `0xff0000ff` from all three
render slots.

The complete enabled Render suite now reaches every test safely. Renderer-count
and primitive tests pass; packed-texture `render_testBlit` and
`render_testBlitColor` remain failed. The first blit mismatch reads transparent
black where the reference is opaque yellow. That coordinate is covered by
multiple copies of the alpha-bearing face texture; a later transparent border
texel must preserve an earlier opaque texel through source-alpha blending.
Consequently, that mismatch alone cannot distinguish failed sampling from
failed blending. A focused diagnostic confirmed the source texel, descriptor,
modulation, copy geometry, UV varying, and readback paths, while an earlier
single full-surface target-texture run returned the expected non-black sampled
color. The guarded packed and zero-alpha probes above are now the required
next qualification gates before changing PSBC or resource-table binding.
Target textures, texture modulation/blending, the native YUV matrix, and longer
recreation/submission stress remain subsequent qualification gates.

### Recovering a stale WebSrv application

If a failed raw-ELF test leaves a black screen, do not launch another renderer
test until a read-only ps5debug-NG process query confirms that the old process
is gone. Do not attach the debugger to an `eboot.elf` that may be partway
through SystemService teardown. WebSrv reports
these payloads as `eboot.elf` in the process list even though kernel EXEC lines
refer to `/app0/eboot.bin`; qualification scripts therefore check both exact
names.

One failed shader-diagnostic run on 2026-07-29 ended with an idle graphics
queue (`rptr == wptr`) and no captured application fatal, GFX reset, or power
event. The runner then attempted a ps5debug-NG attach/kill while the failed
raw application was leaving, the attach returned `ERROR`, and the console
subsequently kernel-panicked and shut down before any cleanup payload was
launched. The panic itself was not present in the bounded klog, so the attach
race is the leading cause rather than a proven kernel stack trace. The runner
no longer performs that operation.

SDL's `testautomation` returns its result through the PS5 SDL2main wrapper
instead of calling libc `exit()`. This is required even on assertion failure:
returning lets SDL2main request `sceSystemServiceKillApp`, while `exit()` can
leave the raw WebSrv application alive and owning a black screen.

The guarded recovery payload already available in the adjacent Vulkan-PS5
workspace is:

```
../Vulkan-PS5/build-prospero-m2/vulkan_ps5_process_cleanup.elf
```

Its source is `Vulkan-PS5/examples/process_cleanup/main.c`, and its CMake target
is `vulkan_ps5_process_cleanup`. The local artifact qualified on 2026-07-29 has
SHA-256 `9fd6b41cf2ea87989c4217234c6f34c96a1ca5dc482355af1258539db77d4d76`.
It refuses to act unless exactly one *other* `eboot.elf` exists. It first asks
SystemService to terminate that app and sends `SIGKILL` only if the same PID
remains. Upload and start it through WebSrv homebrew, not `prospero-deploy`:

```sh
PS5_HOST=10.0.1.41
CLEANUP_ELF=../Vulkan-PS5/build-prospero-m2/vulkan_ps5_process_cleanup.elf
curl -sS "ftp://${PS5_HOST}:2121/" \
  --quote "MKD /data/homebrew/sdl_ps5agc_cleanup" >/dev/null || true
curl -sS -T "$CLEANUP_ELF" \
  "ftp://${PS5_HOST}:2121/data/homebrew/sdl_ps5agc_cleanup/eboot.elf"
curl -sS --get "http://${PS5_HOST}:8080/hbldr" \
  --data-urlencode pipe=1 --data-urlencode daemon=0 \
  --data-urlencode path=/data/homebrew/sdl_ps5agc_cleanup/eboot.elf \
  --data-urlencode cwd=/data/homebrew/sdl_ps5agc_cleanup \
  --data-urlencode args=
```

`process-cleanup: refusing stale eboot count/status=1` means no other stale
`eboot.elf` remained when the recovery payload inspected the process table; it
is the helper's encoded zero-match result, not a process count of one. It then
terminates itself without targeting anything. WebSrv may leave the HTTP
pipe open until the caller's timeout after printing that line; the timeout does
not mean a renderer was found or killed. Keep this payload as a manual recovery
tool only—the automated qualification runner does not launch it.

Renderer presentation is fail-stop: a bounded OpenAGC GPU wait or VideoOut
presentation failure is returned to SDL and prevents any later submission from
reusing a buffer whose state is unknown. Renderer teardown closes VideoOut,
shuts the OpenAGC driver down, and only then releases SDL's GPU-visible pools.
Resource-usage bookkeeping for a draw batch is transactional: target and
sampled-texture usages are committed only after the batch reaches its bounded
fence. If command recording fails before submission, the discarded batch
cannot leave SDL claiming that an unexecuted transition changed GPU state.
The same rule applies to texture cache publication, target/display readback,
and the transition into presentation: SDL commits each new usage only after
the corresponding flush or bounded GPU submission succeeds. A failed flush or
submission therefore retains the last state known to have completed rather
than recording work that never became visible to the GPU.
The software fallback also gives its flip-event wait a one-second bound, so a
missing VideoOut event becomes an SDL error instead of trapping an application
indefinitely on a black screen. Because OpenAGC currently exposes FIFO/VSYNC
presentation only, `SDL_RenderSetVSync(renderer, 0)` returns the renderer's
unsupported-mode error and restores SDL's prior VSYNC state instead of
pretending that presentation changed. Every guarded `testyuv` qualification
launch checks this contract before drawing.

`test/run_ps5agc_yuv_matrix.sh` runs 12 isolated WebSrv launches covering IYUV,
YV12, NV12, and NV21 under JPEG, BT.601, and BT.709 conversion. Each launch
updates the 555x333 streaming texture through an odd `1,1 553x331` rectangle
using deliberately non-tight Y and chroma pitches, renders it into an ABGR8888
target, and compares GPU center readback with SDL's CPU conversion within three
units per channel. The matrix retains the display probe's stale-process,
PID-scoped fatal/reset, power-transition, self-exit, and WebSrv health checks.
SDL's software NV updater now derives interleaved-chroma offsets from `x/2` and
`y/2`, matching its planar updater and the native renderer for odd rectangles.
The firmware 5.50 qualification completed through `KillApp()` followed by
`All processes exited`, with no app crash, fatal signal, GFX reset, or stale
native-game process. PS5 executables use SDL2main's non-returning SystemService
self-termination path; returning from a raw WebSrv-loaded ELF can otherwise
fall through its entry trampoline and raise a post-test page fault.

Installed static CMake targets discover `OpenAGC::openagc` transitively.
`sdl2.pc` and `sdl2-config` include `SDL2main` for PS5's required
SystemService entry/exit wrapper, followed by the SDL archive and its platform
dependencies. Their static link flags also include `openagc`, `kernel`, and
`SceAgcDriver`. A Release OpenAGC-enabled install was validated by linking PS5
PIE consumers through the installed CMake targets, `sdl2.pc`, and the
relocatable `sdl2-config` script.

The current local baseline also completes full Prospero test builds with
OpenAGC enabled and disabled, passes all 17 host CTests, passes the 32-cycle
software texture-churn control, and parses every guarded PS5 qualification
runner with `sh -n`. Qualification counts and timeouts accept canonical
positive decimal integers only; zero and zero-prefixed values are rejected
before a build, upload, or WebSrv launch. These checks validate builds and test
orchestration only; the guarded WebSrv renderer and stress matrices still
require an explicitly available PS5.
