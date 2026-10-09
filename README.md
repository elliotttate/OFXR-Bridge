# OFXR Bridge: native DLSS Frame Generation fork

OFXR Bridge makes VR games look smoother when your PC cannot render as
many frames as your headset displays. For every frame the game renders, it
creates an extra in-between frame, so the headset gets **almost twice as
many frames**, or **almost three times** with 3X Frame Gen. The goal is to
reach your headset's refresh rate: a game running at 45 fps on a 90 Hz
headset gets close to 90 frames a second.

It is "almost" because generating frames costs some GPU time, which lowers
the game's own frame rate a little. Generated frames can also show small
artifacts around fast-moving objects. OFXR runs in the background from a
tray icon, and you switch it on or off there. It works with OpenXR games
only.

Technically, OFXR Bridge is an experimental OpenXR API layer. It generates
frames between the ones the game renders, from optical flow or, when the game
uses DLSS, from the game's own motion vectors.

## About this fork

This is [elliotttate/OFXR-Bridge](https://github.com/elliotttate/OFXR-Bridge).
It is a fork of [djules75/OFXR-Bridge](https://github.com/djules75/OFXR-Bridge),
which maintains the 0.2.x line. That project is itself a fork of
[tig3rmast3r/OFXR-Bridge](https://github.com/tig3rmast3r/OFXR-Bridge), the
original OFXR. The fork's work is on the `native-dlss-fg` branch. It sits on
top of upstream release v0.2.13.1 (internal build V438) and builds as internal
version **V439**.

The fork adds **native NVIDIA DLSS Frame Generation** (NGX DLSS-G) as a VR
frame-generation backend. It sits alongside the methods OFXR already had:

| Method | What makes the in-between frame |
|---|---|
| OFXR, FidelityFX optical flow | AMD FidelityFX optical flow, on any supported D3D12 GPU |
| OFXR, NVIDIA optical flow | the Optical Flow hardware engine of Turing and newer NVIDIA GPUs |
| OFXR + DLSS vectors | OFXR's own synthesis, driven by the game's DLSS motion vectors |
| **Native DLSS FG** (new) | NVIDIA's DLSS Frame Generation feature, fed the game's DLSS motion vectors and depth |

In native-enabled builds, it also makes OFXR + DLSS vectors work without a
separate guide provider. It fixes a hang under SteamVR, corrects the flight
recorder's GPU timings, and adds benchmark and quality tooling. The rest of
OFXR Bridge is unchanged from upstream: the tray, Prefer FPS over latency,
3X Frame Gen, Lower VRAM, the D3D11 and Vulkan bridges, SteamVR pacing, the
FPS overlay and the flight recorder. See [Using OFXR Bridge](#using-ofxr-bridge).

> [!WARNING]
> Native DLSS Frame Generation is **experimental**. It is off in the default
> build and in CI builds, so you have to build it yourself. It needs an NVIDIA
> RTX GPU and a game that uses DLSS. So far it has been tested on one RTX 5090.

## Native DLSS Frame Generation

### How it works

- **The game's DLSS guides are captured from its upscaler.** A native-enabled
  layer hooks the game's NGX calls. It captures the motion vectors and depth
  that the game hands to its D3D12 DLSS or Ray Reconstruction upscaler, along
  with their rectangles, motion scale, jitter and depth flags. Camera depth
  metadata comes from OpenXR depth submissions. When those are missing, it
  comes from UEVR's public projection API. A game with neither still states
  its depth direction in its DLSS flags, and that direction is all NGX's
  output depends on, so nominal planes stand in. A cooperating producer can
  supply the same guides through the V2 guide publication API instead.
  Snapshots copy only the rectangle DLSS reads, and only the depth plane.
- **NGX finds the bundled `nvngx_dlssg.dll` even when the game started NGX
  first.** NGX looks for a feature's DLL only along the search paths of the
  process's first NGX initialisation, which is normally the game's own DLSS.
  A game that ships no `nvngx_dlssg.dll` would then leave frame generation
  "not found" for OFXR. So, when the OpenXR instance is created with a DLSS
  method selected, the layer adds its own folder to the end of every NGX
  initialisation's search paths. Engines create the instance before their
  renderer, so this comes before the game's DLSS starts. A game's own copy
  of the DLL, earlier in its list, still wins.
- **Both eyes share one NGX feature.** The eyes are packed side by side into
  one double-width feature, with a 64-pixel seam that repeats each eye's edge.
  Most of an NGX evaluation's cost is fixed, so one wide evaluation costs far
  less than one per eye. The seam stops either eye's history from reaching the
  other. NGX refuses features wider than 8192 pixels, so for very wide eyes
  the seam narrows, down to 16 pixels. Eyes from 4089 pixels wide get a
  feature each.
- **The previous frame is aligned into the new camera.** The previous real
  frame is rotated into the new frame's camera. The same mapping removes
  tracked head rotation from the game's vectors. When the alignment moves any
  pixel by more than a tenth of a pixel, NGX's history is reset. It is then
  reseeded with the aligned previous frame before the new frame is evaluated.
  A still head, or a translation alone, keeps the history.
- **2X and 3X.** NGX makes one frame at 1/2, or two frames at 1/3 and 2/3 with
  OFXR's 3X Frame Gen setting. 3X needs NGX multi-frame generation. On
  adapters without it, native mode stays at 2X.
- **The real frame stays bit-exact.** Only the in-between frames are
  generated, and OFXR's bit-exact copy of the real frame is preserved. Colour
  reaches NGX display-encoded: sRGB is encoded in the pack and decoded on
  composition, and unchanged pixels round-trip exactly.
- **It fails safe.** When guides are missing or reset, the headset shows the
  current frame. If NGX refuses to create or evaluate a feature, that pair
  shows the current frame and creation is retried after 120 pairs. OFXR never
  calls `NVSDK_NGX_D3D12_Shutdown1`, because the driver shares one NGX
  instance per adapter. Calling it would also shut down the game's own DLSS.

### Requirements

- An NVIDIA RTX GPU and driver for which NGX reports DLSS Frame Generation
  available. Testing so far: an RTX 5090 with driver 616.56.
- A game that uses a D3D12 DLSS or Ray Reconstruction upscaler, for its motion
  vectors and depth. A producer using the V2 guide API also works.
- Everything the normal build needs (see [docs/BUILDING.md](docs/BUILDING.md)):
  Visual Studio 2022, CMake 3.24 or newer, a Windows SDK with `fxc.exe`, the
  FidelityFX SDK v1.1.4 and `openvr.h`.
- The **NVIDIA DLSS SDK, 310.6 or later**. The tested version is 310.9.1, at
  commit `374959484e79a640feaba44c93ac8cfb0a03f5b5` of
  [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS). Older SDKs still build, but
  the `nvngx_dlssg.dll` in 310.6 and later generates the same pixels about 4%
  faster than 310.5.3 at 3004x3004 (2% for a turning head). 310.6, 310.7 and
  310.9.1 measure alike. Native builds also fetch pinned SafetyHook v0.6.9 and
  its Zydis dependency.

### Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DXRFG_NATIVE_DLSSG=ON -DXRFG_DLSS_SDK_ROOT=path/to/DLSS
cmake --build build --config Release
```

`XRFG_NATIVE_DLSSG` is `OFF` by default. `XRFG_DLSS_SDK_ROOT` defaults to
`external/DLSS`, which git ignores. The SDK folder must contain:

```text
include/nvsdk_ngx_helpers_dlssg.h
lib/Windows_x86_64/x64/nvsdk_ngx_d.lib
lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib
lib/Windows_x86_64/rel/nvngx_dlssg.dll
```

The build output in `build/Release` looks like this:

```text
OFXRBridgeTray.exe
ofxr/
  XR_APILAYER_XRFrameBridge_diagnostic.dll
  ofxr_bridge.ini
  nvngx_dlssg.dll
licenses/
  NVIDIA-DLSS.txt, plus the SafetyHook, Zydis, Zycore and UEVR API notices
```

When you copy the tray build, keep `nvngx_dlssg.dll` with the other files in
`ofxr`, together with `licenses/NVIDIA-DLSS.txt`. When the tray arms, it
installs that DLL beside its cached layer. Do not distribute PDBs, static
libraries, test executables or the NVIDIA SDK.

### Turning it on

In the tray menu, select **NVIDIA DLSS Frame Generation (experimental)**
before the game starts its OpenXR session. The choice applies to the next
session. Select **OFXR frame generation** to go back to OFXR's own methods.
While native generation is selected, the tray tooltip shows "native DLSS FG".
The **3X Frame Gen** option switches native generation to 3X.

| Where | Setting |
|---|---|
| Tray settings (`%LOCALAPPDATA%\OFXR Bridge\tray.ini`) | `[tray] frame_generation=dlss` |
| A directly loaded layer (`ofxr_bridge.ini`) | `[ofxr] frame_generation=dlss` |
| V2 provider control | `frame_generation=1` (`0` selects OFXR) |

The default is `frame_generation=ofxr`. With the tray, close it before you
edit `tray.ini`. Edit `tray.ini` rather than the `ofxr_bridge.ini` beside the
tray, because the tray rewrites the layer's settings from `tray.ini` each
time it arms. The optical-flow backend, preset, scale and bidirectional
controls only configure OFXR's own algorithm. They do not tune NVIDIA's
feature.

**Optional: faster native generation at a lower resolution.** The tray's
**DLSS Frame Generation resolution** submenu offers 100% (the default) and
67%, applied the next time the game starts. At 67%, NVIDIA generates at two
thirds of each eye's resolution, and the bridge puts back the real frame's
detail wherever it can follow the game's motion. Moving content in the
generated frames is softer; occlusion edges soften most. In return, Galactic
Racer on a Steam Frame (3004x3004 per eye, 120 Hz, racing) rendered 106.3
game frames a second at 67%, against 99.5 at 100% (OFXR + DLSS vectors:
112.5). Offline, a turning-head 2X pair costs 1.74 ms at 67%, against 2.35 ms.
67% suits games whose DLSS renders at two thirds (Quality).

There is no lower step in the tray. At 50% (107.8 game frames a second), native
generation's moving edges measured no better than OFXR + DLSS vectors', and
everything else worse, for more GPU time. If you want a cheaper method than 67%,
use OFXR + DLSS vectors.

| Where | Setting |
|---|---|
| Tray settings | `[tray] dlssg_resolution=67` |
| A directly loaded layer | `[ofxr] dlssg_resolution=67` (25 to 100) |
| A running game | `OFXR_RequestNativeDlssgScaleV2(67)` |

Environment variables for experiments and diagnostics. Set them before you
launch the game:

| Variable | Effect |
|---|---|
| `XRFG_NATIVE_DLSSG_PER_EYE=1` | A feature per eye instead of the shared one. |
| `XRFG_NATIVE_DLSSG_SEAM` | Overrides the seam width. Without a seam the eyes visibly bleed into each other. |
| `XRFG_TEST_NATIVE_DLSSG_DECISIONS=1` | Logs the GPU's choice for each pair, generated image or current frame, to `ofxr-native-decisions-pid*-instance*.log` beside the layer DLL. It is not a frame-rate benchmark. |
| `XRFG_TEST_NATIVE_DLSSG_RESEED_EVERY_PAIR` | Makes every pair take the turning-head reseed, so its cost can be measured with the headset still. |

## Other changes in this fork

- **OFXR + DLSS vectors without a guide provider.** NGX guide capture now
  runs whenever DLSS motion vectors are selected, not only for native
  generation. In a native-enabled build, OFXR's own synthesis
  (`motion_vectors=dlss` with `frame_generation=ofxr`) therefore uses the
  game's vectors. Before this fix, in Galactic Racer under UEVR it never
  used them and silently ran plain optical flow.
- **Side-by-side stereo with per-eye guides.** UEVR submits both eyes side by
  side in one texture, while DLSS guides arrive per eye. The game-motion path
  rejected every such pair as a temporal mismatch. Each view now uses its own
  eye's guide, as native generation does.
- **DLSS features created before the hook.** Capture recovers a feature the
  game created before the hook was installed, from its evaluation
  parameters.
- **A hang fixed under the SteamVR presenter.** Changing the frame-generation
  method from the tray while Galactic Racer ran on SteamVR hung the game.
  Frames the presenter still held, made by the old synthesizer, waited on
  fence values the new one would never reach. Every control change now drains
  the presenter first. As a second guard, the copy and queue-synchronisation
  paths skip fence values that were never issued. The new test
  `xrfg_layer_steamvr_live_switch` covers this.
- **A GPU fault fixed on resolution changes.** Changing UEVR's resolution
  mid-race with native generation faulted the GPU while the swapchains were
  recreated. The new swapchain's feature initialised NGX a second time while
  the game recreated its own DLSS features. NGX now stays initialised for the
  device. Swapchain teardown also no longer frees the hand-over copier's work
  that has not finished.
- **Cheaper guide snapshots.** Snapshots copy only the rectangle each DLSS
  evaluation reads, and only the depth plane of a depth-stencil target. In
  Galactic Racer, the game-side cost per frame went from 165 us to 65 us on
  an RTX 5090.
- **Flight recorder GPU timings fixed.** DLSS-vector and repeated pairs
  reported zero. The FidelityFX path resolved NVIDIA stage marks it never
  wrote, which raised a D3D12 debug-layer error. The first record read back
  could be all zeros. Each path now resolves only the spans it wrote.
- **Benchmark and quality tooling.** There are benchmarks for every method,
  with a configurable eye size, plus a native layout benchmark and a
  rotation-quality sweep. See [Testing](#testing).

## Performance

All figures were measured on one RTX 5090 with DLSS SDK 310.9.1. Each is the
median GPU time per stereo pair. These are measurements from one machine, not
guarantees.

**Offline, by eye size.** These come from `XRFG_TEST_FG_BENCH`, run through
the synthesizer the layer uses. The head is turning, guides are at two thirds
of the eye size, frames are patterned, and no VR game is running.

| Method | 1440x1584 | 2004x2004 | 3004x3004 | 3600x3600 |
|---|---|---|---|---|
| OFXR + DLSS vectors | 0.13 ms | 0.22 ms | 0.50 ms | 0.71 ms |
| OFXR FidelityFX, half-res flow | 0.20 ms | 0.31 ms | 0.58 ms | 0.78 ms |
| OFXR FidelityFX, full-res flow | 0.38 ms | 0.64 ms | 1.30 ms | 1.80 ms |
| Native DLSS FG 2X | 1.14 ms | 1.49 ms | 2.38 ms | 3.42 ms |
| OFXR NVIDIA medium flow | 1.30 ms | 1.61 ms | 2.89 ms | 3.80 ms |
| Native DLSS FG 3X | 1.66 ms | 2.13 ms | 3.73 ms | 5.02 ms |

**Live, Galactic Racer on a Steam Frame.** The game ran under UEVR through
SteamVR, at 3004x3004 per eye and 120 Hz, racing the Arcade time trial at
speed. Each method had four interleaved rounds of ten seconds.

| Method | GPU per stereo pair (median) | Game frames a second |
|---|---|---|
| OFXR + DLSS vectors | 0.44 ms | 117.8 |
| OFXR FidelityFX flow | 0.90 ms | 113.7 |
| Native DLSS FG 2X | 4.9 ms | 109.8 |
| OFXR NVIDIA medium flow | 7.8 ms | 113.5 |

- In the headset, the GPU is shared with the game. Offline at that size, the
  same native pair takes 2.4 ms, so about half of the live figure comes from
  that sharing.
- NVIDIA flow's long span runs largely on the optical-flow engine, beside the
  game's rendering.
- On a second track, Tatooine's King of the Racers podrace, the release build
  measured the same order: OFXR + DLSS vectors 0.45 ms (116.7 fps), FidelityFX
  flow 0.90 ms (115.5), native 5.2 ms (112.0) and NVIDIA medium flow 6.9 ms
  (115.5).
- The tracks' sections load the GPU differently, so single rounds of a method
  ranged by up to 11 frames a second.
- On the Meta XR Simulator (1440x1584 per eye), every method held the
  simulator's 90 frames a second.

**Live, Hubris.** Hubris is a native Unreal Engine 4 VR game with DLSS 310.2.1
and no DLSS Frame Generation of its own. It ran through SteamVR at 2568x2568
per eye, in its menu scene. The game was not GPU-bound there, so every method
held 119.5-119.9 frames a second. Each figure is the median of a ten-second
round; repeated rounds agreed within 10 us.

| Method | GPU per stereo pair (median) |
|---|---|
| OFXR + DLSS vectors | 0.32 ms |
| OFXR FidelityFX flow | 0.67 ms |
| Native DLSS FG 2X at 67% resolution | 1.40 ms |
| Native DLSS FG 2X | 1.87 ms |
| OFXR NVIDIA medium flow | 3.1 ms |

Hubris needed two fixes to run native at all: the search-path change
described in [How it works](#how-it-works), and depth taken from its DLSS
flags, because it submits no OpenXR depth.

**Where native's time goes.** NGX's own evaluations take most of a pair. At
3004x3004 they take 1.66 of 1.89 ms with a still head, and 2.02 of 2.36 ms
turning. OFXR's own work is the remainder: about 180 us to pack the new
frame, about 105 us for the reseed's colour pack (turning head only) and
about 75 us to compose. That work writes the full-resolution colour, motion
and depth that NGX takes in its own feature layout, and composes NGX's output.
At 3004x3004 that is about 350 MB per pair, so the GPU's memory bandwidth
sets its speed.

## Optimizations, and what was rejected

Changes kept, with their measured effect:

- **One shared side-by-side feature instead of one per eye.** It measured
  cheaper at every size tried: by 29% (still head) and 28% (turning) at
  1440x1584, down to 14% and 9% at 3600x3600.
- **Reseeding only when rotation moves a pixel by more than 0.1 px.** A still
  head or a translation keeps NGX's history.
- **Motion as a fraction of the feature,** with the size of the motion grid as
  NGX's motion scale. The same vectors in pixels with a unit scale lose
  measurable quality.
- **Undilated vectors.** NGX dilates them itself. Quality is the same, and a
  pair costs about 35 us less than dilating them in the pack.
- **DLSS SDK 310.9.1.** It generates identical pixels, faster than 310.5.3.
- **A colour-only reseed in 16x8 thread groups.** NGX's output from a reset
  is bit-identical whatever motion and depth it is given. A turning pair at
  3004x3004 went from 2.58 to 2.49 ms.
- **8-bit packing for 8-bit swapchains, with the reseed kept at 10 bits.** A
  pair is 2-4% faster, and almost every quality test measures less error.
- **A two-thirds guide grid** when the game renders at two thirds of the
  output or less (DLSS Quality and below). A pair at 3004x3004 is 1.8-2.2%
  faster at the median, with the same quality.
- **Leaner native mode.** It skips OFXR's optical-flow contexts, scratch
  textures and extra command lists. Fully covered outputs skip the
  preliminary current-frame copy, and the real-frame copy can be deferred
  until after the generated image is submitted.
- **Occlusion handling for OFXR + DLSS vectors.** It used to show a blend
  of both frames at the same pixel wherever the two warped frames disagreed,
  so both frames' edges at once. Now background a moving edge uncovers comes
  from the new frame, background it is covering comes from the previous one,
  and near a motion edge a short search finds the surface in front. On the
  benchmark scene of a striped square sliding over a detailed background, the
  error fell from 3.95 to 1.81, and at the square's edges from 25.9 to 7.35.
  Native generation measures 4.25 and 8.7 there. At 3X, the frames a third
  and two thirds of the way err 1.50 and 1.39 (native: 3.38 and 2.99; before:
  4.88 and 2.96). With the game's vectors
  exact, OFXR + DLSS vectors is now the more accurate method at about a sixth
  of the GPU time. Native generation still handles content the vectors do not
  describe better. In Hubris the pair cost was unchanged at 0.33 ms. On the
  offline benchmark, whose frames disagree almost everywhere, it rose about
  10%.

Tried and rejected:

- **Keeping NGX's history through small rotations,** and aligning the
  generated image in the compose pass. This saves the reseed, but the error
  was two to five times higher, even at a quarter pixel of rotation.
- **Catmull-Rom instead of bilinear** for the aligned previous frame. It was
  slightly worse (0.36 against 0.32 at the smallest rotation).
- **Skipping the reseed, or raising the 0.1 px threshold.** A quarter pixel
  of unaligned history already triples the error.
- **Running the pack and NGX on an async compute queue.** The output was
  identical, but live on the Steam Frame, in the game's hub scene, the median
  pair was 0.2-0.7 ms slower at every load tried. While racing, the median
  went from 5.1-5.3 ms to 6.3 ms and the game lost about two frames a second.
- **Generating below eye resolution by default.** Upscaling alone lost too
  much detail, so it is an option instead (see
  [Turning it on](#turning-it-on)). There, the bridge restores the real
  frame's detail along the game's motion. On a moving test scene, that takes
  67%'s error from 8.7 to 5.4, against 4.25 at full resolution and 28.8 for
  a blend of the two frames.
- **A half-size guide grid.** Depth edges measured worse.
- **8-bit colour for the reseed too.** It is slightly faster, but the
  rotation sweep's error rises from 0.31 to 0.40.
- **Separate input textures for the reseed.** They are no faster and cost
  about 220 MB more video memory at 3004x3004.
- **32-bit motion.** It is 2% slower, with worse depth edges (0.33 against
  0.21).
- **Settings with no measurable effect:** 16-bit depth, a 16-pixel seam, the
  pack's thread-group shapes, NGX's undocumented `DLSSG.InternalWidth`,
  `DLSSG.DynamicResolution` and `DLSSG.EvalFlags` parameters, and every NGX
  evaluation option.

The full measurements and reasoning are in
[docs/BUILDING.md](docs/BUILDING.md#native-nvidia-dlss-frame-generation).

## Testing

```powershell
# Default build
ctest --test-dir build -C Release --output-on-failure

# Native build, as in docs/BUILDING.md
ctest --test-dir build -C Release --output-on-failure -LE needs_gpu_timing
ctest --test-dir build -C Release -R '^xrfg_native_dlssg_tests$' --output-on-failure
```

In a native build, the first command skips the tests labelled
`needs_gpu_timing`, and the native GPU test is one of them. The second
command runs the native GPU test, `xrfg_native_dlssg_tests`. That test needs
an adapter and driver for which NGX reports frame generation available. It
checks:

- translated stereo quality against a same-pixel blend
- 3X output order
- cropped view bounds
- rotational camera isolation
- command-list reuse
- real-frame copies
- explicit resets
- missing-depth fallback

These small synthetic tests do not establish headset-resolution cost,
headset comfort, or behaviour in a particular game.

**Benchmarks.** Set one of the variables below and run
`build\Release\xrfg_d3d12_history_tests.exe`. A benchmark replaces the normal
test run. Close VR games first, because GPU contention makes the medians
meaningless.

| Variable | Measures |
|---|---|
| `XRFG_TEST_FG_BENCH=1` | Every frame-generation method through the synthesizer, at 2004x2004 per eye by default: OFXR FidelityFX and NVIDIA flow, DLSS vectors, and native 2X/3X. It also times the game-side guide snapshot copies. The native rows need a native build. |
| `XRFG_TEST_NATIVE_DLSSG_BENCH=1` | One native stereo pair, still and turning head, at 2X and 3X, at 2064x2208 per eye by default. |
| `XRFG_TEST_BENCH_EYE=WxH` | The per-eye size for the two benchmarks above, for example `3004x3004`. Guides stay at two thirds of it. |
| `XRFG_TEST_NATIVE_DLSSG_ROTATION_SWEEP=1` | Native quality on a detailed static scene after head rotations of about 0.25 to 16 pixels. |
| `XRFG_TEST_NATIVE_DLSSG_LAYOUT_BENCH=1` | NGX alone, for one eye, for two features, and for one shared feature. |

```powershell
$env:XRFG_TEST_FG_BENCH = '1'
$env:XRFG_TEST_BENCH_EYE = '3004x3004'
.\build\Release\xrfg_d3d12_history_tests.exe
Remove-Item Env:XRFG_TEST_FG_BENCH, Env:XRFG_TEST_BENCH_EYE
```

The native pair and all-methods benchmarks generate from patterned frames.
Blank frames compress to almost nothing in GPU memory, and NGX then measures
20-25% faster than it does on real content.

## Status and caveats

- **Experimental.** It is off in the default build, and CI builds the default
  configuration. Image quality in the headset is still being evaluated.
- **NVIDIA RTX only.** It needs an adapter and driver for which NGX reports
  frame generation available. It has been tested on one RTX 5090.
- **It needs a game that uses DLSS.** Native generation needs complete,
  continuous motion and depth guides for both source frames. Capture works
  with a D3D12 DLSS or Ray Reconstruction upscaler. Other depth conventions
  need explicit metadata or the V2 publication API.
- **UEVR depth.** For UEVR's projection metadata to be used, both eyes'
  reversed, infinite projections must be valid and agree.
- **Motion-only producers.** A V1 producer that sends only motion can keep
  using OFXR. Native mode shows the current frame until depth is available,
  and its statistics report `waiting_for_depth` (status 7).
- **No silent fallback.** If the native feature is unavailable, the session
  fails to initialise and reports the error; it does not quietly switch
  algorithms. If NGX later refuses a feature, the statistics report
  `native_unavailable` (status 8). A 3X request on an adapter without NGX
  multi-frame generation reports `multi_frame_unsupported` (status 9).
- **NGX started before the layer.** The search-path change only works if the
  layer loads before the game's DLSS starts NGX. If UEVR injects a game after
  it has started, and the game ships no `nvngx_dlssg.dll`, native generation
  is unavailable. OFXR + DLSS vectors still works there. If the session starts
  on OFXR's FidelityFX or NVIDIA flow without DLSS vectors, the layer leaves
  the search paths alone, and switching to native needs a game restart.
  Setting `XRFG_TEST_NATIVE_DLSSG_VERBOSE=1` makes the layer record NGX's
  answers in `%LOCALAPPDATA%\OFXR Bridge\NGX\ofxr-native-dlssg.log`.
- **Fixed cadence.** Native mode uses NGX's fixed fractions, so it suits games
  that hold half or a third of the display rate. Away from that cadence, each
  generated image is shown at the wrong instant and motion judders. OFXR's own
  algorithm corrects for this.
- **Camera translation.** Full reprojection of camera translation, and
  separate guides for baked-in HUD and UI, remain future work. The bridge's
  existing limitation for camera translation still applies.
- **Live testing so far** covers Galactic Racer under UEVR (two tracks), on a
  Steam Frame through SteamVR and on the Meta XR Simulator, and Hubris, a
  native Unreal Engine 4 VR game, through SteamVR.

## Using OFXR Bridge

For everyday use, the tray works as it does upstream:

1. Run `OFXRBridgeTray.exe`. The bridge arms itself straight away.
2. Right-click the tray icon to choose the method and options.
3. Start the game normally. For injectors such as UEVR, start the tray before
   the game.
4. Select **Disarm bridge**, or close the tray, when you are finished.

> [!IMPORTANT]
> **OpenXR games only.** Games built on OpenVR, SteamVR's older system, are
> not supported, even though they run on SteamVR. For UEVR, select its OpenXR
> option. Also, **never run the game as administrator.** The OpenXR loader
> ignores per-user layers in an elevated program, so the bridge never loads
> there.

The [upstream README](https://github.com/djules75/OFXR-Bridge#readme) has the
full user guide and explains every tray option. For more, see:

- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)
- [docs/FPS_OVERLAY.md](docs/FPS_OVERLAY.md)
- [docs/releases](docs/releases)

If you report a problem, the flight recorder log helps. Turn on **Bridge
flight recorder** in the tray, reproduce the problem, then use **Open bridge
logs**.

## Credits and license

- **tig3rmast3r** created OFXR:
  [tig3rmast3r/OFXR-Bridge](https://github.com/tig3rmast3r/OFXR-Bridge),
  [ko-fi.com/tig3rmast3r](https://ko-fi.com/tig3rmast3r).
- **Djules** maintains the 0.2.x version that this fork builds on:
  [djules75/OFXR-Bridge](https://github.com/djules75/OFXR-Bridge),
  [ko-fi.com/djules](https://ko-fi.com/djules).

OFXR Bridge is licensed under [LGPL-3.0-or-later](LICENSE). Third-party
components keep their own licenses. See [THIRD_PARTY.md](THIRD_PARTY.md) and
[licenses/](licenses).

Native-enabled builds statically link the NGX D3D12 interface from the NVIDIA
DLSS SDK. They ship NVIDIA's unmodified production `nvngx_dlssg.dll` under
the SDK license in
[licenses/NVIDIA-DLSS.txt](licenses/NVIDIA-DLSS.txt). They also statically
link SafetyHook (Boost Software License 1.0), Zydis and Zycore (MIT), and use
UEVR's public API header (MIT). Their notices are in [licenses/](licenses).
This fork is not sponsored or endorsed by NVIDIA.
