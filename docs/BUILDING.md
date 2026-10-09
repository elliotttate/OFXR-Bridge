# Building OFXR Bridge

OFXR Bridge currently targets 64-bit Windows and builds with Visual Studio
2022, CMake 3.24 or newer and a recent Windows SDK containing `fxc.exe`.

## Dependencies

The repository already contains the exact OpenXR headers and NVIDIA Optical
Flow interface headers used by the project. NVIDIA's runtime API is supplied by
the installed display driver and is not required at build time.

FidelityFX SDK v1.1.4 is intentionally not committed. Clone the official SDK
at the pinned revision and build its static DX12 Optical Flow components:

```powershell
git clone --branch v1.1.4 --depth 1 `
  https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git `
  external/FidelityFX-SDK-v1.1.4

cmake -S external/FidelityFX-SDK-v1.1.4/sdk `
  -B build-fidelityfx -G "Visual Studio 17 2022" -A x64 `
  -DFFX_ALL=OFF -DFFX_OF=ON -DFFX_API_BACKEND=DX12_X64 `
  -DFFX_BUILD_AS_DLL=OFF

cmake --build build-fidelityfx --config Release
```

The expected outputs are:

```text
external/FidelityFX-SDK-v1.1.4/sdk/bin/ffx_sdk/ffx_backend_dx12_x64.lib
external/FidelityFX-SDK-v1.1.4/sdk/bin/ffx_sdk/ffx_opticalflow_x64.lib
```

The pinned FidelityFX commit is
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.

## Build and test

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The distributable files are generated under `build/Release`:

```text
OFXRBridgeTray.exe
ofxr/
  XR_APILAYER_XRFrameBridge_diagnostic.dll
  ofxr_bridge.ini
```

Do not distribute PDBs, static libraries, test executables or the NVIDIA SDK.

## Native NVIDIA DLSS Frame Generation

V439 adds an optional native NGX DLSS FG path. The default build keeps this
option off. Obtain the NVIDIA DLSS SDK and configure its root explicitly.
Native builds also fetch pinned SafetyHook v0.6.9 and its Zydis dependency:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DXRFG_NATIVE_DLSSG=ON -DXRFG_DLSS_SDK_ROOT=E:/Github/DLSS
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -LE needs_gpu_timing
ctest --test-dir build -C Release -R '^xrfg_native_dlssg_tests$' --output-on-failure
```

The native GPU test needs an adapter and driver for which NGX reports frame
generation available. The implementation was tested with SDK 310.9.1 at commit
`374959484e79a640feaba44c93ac8cfb0a03f5b5` of NVIDIA/DLSS and
driver 616.56 on an RTX 5090. Use 310.6 or later: its `nvngx_dlssg.dll`
generates the same pixels about 4% faster than 310.5.3 at 3004x3004 (2% for
a turning head), and 310.6, 310.7 and 310.9.1 measure alike. Older SDKs
still build. SDK directories must contain
`include/nvsdk_ngx_helpers_dlssg.h`, `lib/Windows_x86_64/x64/nvsdk_ngx_d.lib`,
`lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib`,
and `lib/Windows_x86_64/rel/nvngx_dlssg.dll`.

The build copies the production feature DLL next to the layer and into
`build/Release/ofxr`. Keep `nvngx_dlssg.dll` with the other files in `ofxr`
when copying the tray build, together with `licenses/NVIDIA-DLSS.txt`.
The tray installs that DLL beside its cached layer. In the tray, select
**NVIDIA DLSS Frame Generation (experimental)** before starting a new OpenXR
session. **OFXR frame generation** restores the existing algorithm. The
corresponding configuration is `[tray] frame_generation=dlss` in the saved
tray settings, or `[ofxr] frame_generation=dlss` for a directly loaded layer.
The V2 provider control uses `frame_generation=1`; zero selects OFXR.

Native generation requires complete, continuous guide publications for both
source frames: motion, depth, their exact output/resource rectangles, jitter,
scales, and valid near/far and reversed/infinite depth conventions. The V2
guide publication API supplies these inputs. Native builds can also capture
these resources directly from a D3D12 DLSS/Ray Reconstruction upscaler.
Capture uses the NGX feature's rectangles, motion scale, jitter and depth flags;
camera depth metadata comes from OpenXR depth submissions or UEVR's public
SDK projection matrix. Both reversed, infinite UEVR eye projections must be
valid and agree before publication. Separate upscaler eyes can map onto a
single side-by-side OpenXR texture. Other depth conventions still require
explicit metadata or the V2 publication API. A motion-only V1 producer can
continue using OFXR; the native path shows the current frame until depth is
available. Its motion statistics report `waiting_for_depth` (status 7).
An unavailable native feature fails initialization and reports the session
error; it does not silently switch algorithms. If NGX later refuses to create
or evaluate a feature, that pair shows the current frame, the statistics
report `native_unavailable` (status 8), and creation is retried after 120
pairs instead of disabling generation. Separate OpenXR composition layers
continue through the existing layer path.

Capture runs whenever DLSS motion vectors are selected, not only for native
generation, so OFXR's own synthesis (`motion_vectors=dlss` with
`frame_generation=ofxr`) uses the game's vectors without a guide provider.
Like native generation, it pairs each view of a side-by-side texture with its
own eye's guide. In Galactic Racer under UEVR on the Meta XR Simulator
(1440x1584 per eye, simulated head still), median GPU time per stereo pair:

| Method | Racing at speed | 3D scene behind a menu |
|---|---|---|
| OFXR + DLSS vectors | 0.13 ms | 0.12 ms |
| OFXR FidelityFX flow | 0.30 ms | 0.30 ms |
| Native DLSS FG 2X | 1.05 ms | 1.08 ms |
| OFXR NVIDIA medium flow | 2.26 ms | 2.3 ms |

Scene motion leaves the medians unchanged; the heavier race widens the tails
(native's 90th percentile rises from 1.08 to 1.5 ms) through contention with
the game's own rendering. With the final build, four interleaved rounds of the
same race measured OFXR + DLSS vectors 0.125 ms, FidelityFX flow 0.30 ms,
native 1.04 ms and NVIDIA medium flow 2.45 ms, every method holding the
simulator's 90 frames a second.

On a Steam Frame through SteamVR (3004x3004 per eye at 120 Hz, with
`power.pauseCompositorOnStandby` off so the session runs unworn), racing the
Arcade time trial at speed, four interleaved rounds of ten seconds per method
measured:

| Method | GPU per stereo pair (median) | Game frames a second |
|---|---|---|
| OFXR + DLSS vectors | 0.44 ms | 117.8 |
| OFXR FidelityFX flow | 0.90 ms | 113.7 |
| Native DLSS FG 2X | 4.9 ms | 109.8 |
| OFXR NVIDIA medium flow | 7.8 ms | 113.5 |

The track's sections load the GPU differently, so single rounds of a method
ranged by up to 11 frames a second; NVIDIA flow's long span runs largely on
the optical-flow engine beside the game's rendering. Offline at that size
(`XRFG_TEST_BENCH_EYE=3004x3004` with either benchmark) the same native pair
takes 2.4 ms, so about half of the live figure is the GPU shared with the
game. With 310.9.1 a
pair there costs 1.89 ms with a still head and 2.36 ms turning (3X: 2.93 and
3.50 ms), and a feature per eye would cost 17% more still and 11% more
turning. Live in the game's lighter hub scene, where the GPU is not saturated,
the still-head pair measured 1.93 ms. Skipping the reseed instead is worse: a quarter pixel of
unaligned history already triples the error of a reseeded pair on detailed
content, so the 0.1 pixel threshold stays.

A DLSS feature the game created before the capture hook was installed is
recovered from its evaluation parameters, which normally still hold its
creation flags. Without them, capture assumes render-resolution motion and
takes the depth convention from the camera metadata.

OFXR never shuts NGX down. The driver keeps one NGX instance per adapter for
the whole process, and `NVSDK_NGX_D3D12_Shutdown1` shuts down every loaded
feature module for that device, including the game's own DLSS upscaler.

Both eyes of a stereo pair share one native feature, side by side, with a
64-pixel seam between them that repeats each eye's edge. Most of an NGX
evaluation's cost is fixed, so one double-width evaluation costs far less than
one per eye; the seam keeps either eye's history from reaching the other.
A single eye, or `XRFG_NATIVE_DLSSG_PER_EYE=1`, gives each eye its own feature
at its submitted viewport size, and a shared feature NGX refuses falls back to
that automatically. NGX refuses features wider or taller than 8192 pixels, so
eyes too wide for both and a full seam narrow the seam to fit, down to 16
pixels, and wider eyes (from 4089 pixels) get a feature each without a refused
pair. The shared feature measured cheaper than one per eye at every size
tried: by 29% (still head) and 28% (turning) at 1440x1584, 18% and 14% at
2448x2448, 17% and 11% at 3004x3004, and 14% and 9% at 3600x3600. `XRFG_NATIVE_DLSSG_SEAM` overrides the seam width for
experiments; without a seam the eyes visibly bleed into each other.

A is rotationally mapped into B's camera plane; the same mapping removes
tracked rotation/FOV motion from engine vectors. When that alignment moves any
pixel by more than a tenth of a pixel, the feature is reseeded with the
aligned A before evaluating B; a still head or a translation alone keeps the
history. A reset clears a shared feature's history for both eyes, so a reseed
packs the aligned A of both. It packs only A's colour: NGX's output from a
reset is bit-identical whatever motion and depth it is given, even noise
(checked with SDK 310.5.3 and 310.9.1), so the seed writes neither and the
reset reads a 64-pixel square of them. With the seed's own 16x8 thread groups,
which suit its rotated bilinear reads better than the pack's 8x8, a turning
pair at 3004x3004 costs 2.49 rather than 2.58 ms; a still head is unchanged.
Keeping the history through small rotations instead, and aligning the
generated image into B's camera in the compose pass, saves the reseed but
measured two to five times the error on detailed content even at a quarter
pixel of rotation; `XRFG_TEST_NATIVE_DLSSG_ROTATION_SWEEP=1` repeats that
quality measurement for the reseeding path.
This preserves OFXR's current B pose/FOV contract and its bit-exact real-frame
copy. On an RTX 5090 at 2064x2208 per eye with SDK 310.9.1, a stereo pair
takes about 1.32 ms of GPU time at 2X with a still head and 1.63 ms with a
turning head, and 2.00/2.36 ms at 3X; through the synthesizer at 2004x2004 a
2X pair takes 1.48 ms. Both benchmarks generate from patterned frames: blank
ones compress to almost nothing in GPU memory, and NGX then measures 20-25%
faster than it does on real content. Set `XRFG_TEST_NATIVE_DLSSG_BENCH=1`
and run `xrfg_d3d12_history_tests` to repeat that measurement;
`XRFG_TEST_NATIVE_DLSSG_LAYOUT_BENCH=1` times NGX alone for one eye, two
features and one shared feature. `XRFG_TEST_FG_BENCH=1` instead times every
frame-generation method through the synthesizer (OFXR FidelityFX and NVIDIA
flow, DLSS vectors, native 2X/3X) at 2004x2004 per eye, plus the game-side
guide snapshot copies. Close VR games first: GPU contention makes the medians
meaningless. Its median GPU time per stereo pair on an RTX 5090, by eye size
(`XRFG_TEST_BENCH_EYE`):

| Method | 1440x1584 | 2004x2004 | 3004x3004 | 3600x3600 |
|---|---|---|---|---|
| OFXR + DLSS vectors | 0.12 ms | 0.21 ms | 0.45 ms | 0.63 ms |
| OFXR FidelityFX, half-res flow | 0.20 ms | 0.31 ms | 0.58 ms | 0.78 ms |
| OFXR FidelityFX, full-res flow | 0.38 ms | 0.64 ms | 1.30 ms | 1.80 ms |
| Native DLSS FG 2X | 1.14 ms | 1.49 ms | 2.38 ms | 3.42 ms |
| OFXR NVIDIA medium flow | 1.30 ms | 1.61 ms | 2.89 ms | 3.80 ms |
| Native DLSS FG 3X | 1.66 ms | 2.13 ms | 3.73 ms | 5.02 ms |

NGX receives colour display-encoded, as its programming guide requires, and
motion as a fraction of the feature with the feature's size as its motion
scale. The same vectors in pixels with a unit scale measurably lose quality.
The vectors are passed undilated and NGX dilates them at depth edges; dilating
them in the pack measured the same and cost about 35 us more per pair.
When the game renders both guides at two thirds of the output or less (DLSS
Quality and below), they are packed to a grid two thirds the feature's size,
which loses none of their detail, and NGX is told that render size. A pair at
3004x3004 is then 1.8-2.2% faster at the median and the quality tests measure
the same; a half-size grid measured worse depth edges, and NGX's motion scale
must then be the grid's size, not the output's.

Of a 2X pair at 3004x3004, OFXR's own work is the pack of B (about 180 us),
the reseed's colour-only pack of the aligned A (about 105 us, turning head
only) and the composition (about 75 us); NGX's evaluations are the rest (the
reset's about 250 us). With SDK 310.9.1, NGX's evaluations alone take 1.21 ms of a 1.32 ms
pair at 2064x2208 (1.47 of 1.63 ms turning), and at 3004x3004 1.66 of 1.89 ms
(2.02 of 2.36 ms). The
remainder writes the full-resolution colour, motion and depth NGX takes in its
own feature layout and composes its output, about 350 MB per pair at
3004x3004: as long as the RTX 5090's memory bandwidth needs to move it. With
patterned frames, 16-bit depth for NGX and a 16 rather than 64 pixel seam
still measure the same. NGX evaluates 8-bit colour faster than 10-bit, so an
8-bit swapchain's frames are packed at 8 bits, which hold their codes exactly,
while the reseed's resampled A keeps 10 bits in a texture of its own (about 73
MB at 3004x3004). That makes a pair 2-4% faster than 10 bits throughout and
lowers the error of almost every quality test (the rotation sweep's from 0.32
to 0.31; two sRGB stereo cases move by 0.0001); NGX takes the differently
formatted reset with SDK 310.5.3, 310.6 and 310.9.1 alike. 8 bits throughout
would be 3-4% faster still, but rounding the resampled A raises the sweep's
error to 0.40. Other swapchain formats keep 10 bits. These also measured no faster or slower, within about 15 us
at 3004x3004: 16x16, 32x8 or 16x8 groups for B's pack (wider groups help only
the seed), NGX's undocumented `DLSSG.InternalWidth` (50 and 75%),
`DLSSG.DynamicResolution` and `DLSSG.EvalFlags` (0, 1, 2, 4, 8) parameters,
every evaluation option (`notRenderingGameFrames`, `menuDetectionEnabled`,
`colorBuffersHDR`, `cameraMotionIncluded`, `orthoProjection`,
`automodeOverrideReset`, and `minRelativeLinearDepthObjectSeparation` at 1 and
1000), and giving the seed its own input textures so both packs run before
either evaluation, which would also cost about 220 MB more video memory. On
blank frames, 32-bit motion was slower. These feature versions accept only
render preset 1.

Generating below the eye's resolution and upscaling the result is cheaper -
at 3004x3004 on blank frames, 75% per axis took a still/turning pair from
1.66/2.04 ms to 1.28/1.61 ms and 67% to 1.11/1.36 ms - but it is a trade, not an
optimisation: on the rotation sweep's detailed scene, the down-and-up resample
alone adds an error of 1.2 (75%) to 1.4 (67%), four times what a native pair
otherwise shows (0.3), on every generated frame between sharp real ones.

Running the pack and NGX on a compute queue, which NVIDIA can overlap with the
game's graphics where queues of the same type time-slice, and composing on the
synthesis queue once a fence passes, was also tried. NGX accepts a compute list
and the output was identical, but the split never helped. Live in Galactic
Racer on the Steam Frame, interleaved with the current path, it added 0.2 to
0.7 ms to the median pair at every load tried in the hub scene. With the scene
heavy enough to hold the game at about 90 of 120 frames a second (UEVR
resolution scale 1.19), the game's frame rate and the number of pairs delivered
were unchanged, and the 90th percentile rose from about 5.4 to 8.2 ms. Racing
the Arcade time trial at speed, the median pair went from 5.1-5.3 to 6.3 ms,
the 90th percentile from 5.8 to 7.9 ms, and the game lost about two frames a
second. Generation therefore stays on the one high-priority direct queue.

sRGB swapchains are encoded by the pack shader into 8-bit private textures
(the reseed's into 10-bit ones, or 8-bit where the adapter lacks typed UAV
stores for 10-bit), and decoded again when the generated image is written;
unchanged pixels round-trip exactly. Resize retirement polls the previous completion
fence, and frame submission does not wait on the CPU. Generated pixels remain
inside the submitted rectangles. NGX's disable-interpolation output is checked
on the GPU before presenting a generated image.

For a live diagnostic run, set `XRFG_TEST_NATIVE_DLSSG_DECISIONS=1` before
launching the game. This adds asynchronous readback of those GPU decisions to
`ofxr-native-decisions-pid*-instance*.log` beside the layer DLL. Each completed
record identifies the guide publication, eye count, output count and flags.
A zero low byte selects generated pixels; a nonzero low byte selects the
current frame. Inspect the file contents while the game runs: Windows can
report a stale directory-entry file size until the writer closes it. This
diagnostic is off by default and is not a frame-rate benchmark.

NGX uses fixed interpolation fractions: one generated image at 1/2, or two at
1/3 and 2/3 for OFXR's 3X setting, subject to the native feature's capabilities.
The layer uses those fractions in native mode. OFXR's cadence-derived arbitrary
fractions remain available with its original algorithm. Native mode is most
appropriate when the application holds half or a third of the display rate:
away from that cadence each generated image is shown at the wrong instant and
motion judders, which the OFXR algorithm corrects for. 3X needs NGX
multi-frame generation; on adapters without it, native mode stays at 2X, and
a 3X request that reaches the feature anyway reports
`multi_frame_unsupported` (status 9).
The optical-flow preset, scale and bidirectional controls configure the
original OFXR algorithm; they do not tune NVIDIA's neural feature.

Native mode skips the original optical-flow contexts, scratch textures and
extra command lists. Fully covered outputs also skip the preliminary current
frame copy; cropped outputs retain it to preserve pixels outside the viewports.
The real-frame copy is recorded separately and can be deferred until after the
generated image is submitted. Packing the aligned previous-frame seed omits
motion-vector depth dilation. These changes preserve the input resolution and
camera alignment. Completed native pairs resolve GPU timestamps for the total
synthesis span; the per-stage optical-flow timings do not describe NGX internals.

The game's FPS counter measures source frames. OFXR's generated frames are
submitted through OpenXR and do not increment that counter. At 120 Hz, 2X mode
paces source frames toward 60 FPS, subject to rendering and generation cost.
Compare on/off in the same scene and resolution, with diagnostic recording off
and an active headset. SteamVR standby and its dashboard can throttle the source
to roughly 100 ms waits; discard those samples. Compositor presents also include
reprojection, so they alone do not establish the rate of unique generated images.

The hardware test checks translated stereo quality against a same-pixel blend,
3X output order, cropped view bounds, rotational camera isolation, command-list
reuse, real-frame copies, explicit resets, and missing-depth fallback. The
shared tests also verify typed depth snapshots preserve values while allowing
shader reads. These small synthetic tests do not establish headset-resolution
cost, headset comfort, or behavior in a particular game. Full camera translation
reprojection and separate baked-in HUD/UI guides remain future work; the bridge
still has its existing limitation for camera translation.

## Continuous integration

`.github/workflows/build.yml` runs the same steps on a clean `windows-2022`
runner for every push to `main`, every pull request and every tag: it fetches
the FidelityFX SDK at the pinned commit and `openvr.h` at the pinned SHA-256
(a mismatch fails the job), builds, runs `ctest` on WARP, and uploads the file
set the release zips carry, named after the version stamp and the commit. The
FidelityFX libraries are cached between runs, so only the first run after a
pin change pays for that build. Tag builds are the input to code signing
through the SignPath Foundation; the `sign` job is inert until the repository
variables it names are set.

## OpenVR header

**Required for the layer.** The overlay reports what the headset actually
received, not what the layer submitted, and SteamVR only exposes that through
OpenVR — `IVRCompositor::GetCumulativeStats`, the same source fpsVR reads. No
OpenXR call reports it. The standalone `xrfg_steamvr_delivery_probe` uses the
same header.

The Vulkan headers the Vulkan interop compiles against are vendored under
`external/Vulkan-Headers` (Khronos Vulkan-Headers `v1.3.296`, listed in
`THIRD_PARTY.md`); nothing further to install, and no Vulkan library is
linked.

Place a single header at `external/openvr/openvr.h`:

```powershell
curl -sSL -o external/openvr/openvr.h `
  https://raw.githubusercontent.com/ValveSoftware/openvr/v2.5.1/headers/openvr.h
```

| File | Tag | SHA-256 |
| --- | --- | --- |
| `openvr.h` | `v2.5.1` | `94E5545370159C85F87CD6E15DD3739F7C919FC7A6E869F5E4ED463533A07ED0` |

Like the FidelityFX SDK, the checkout is intentionally not committed, and the
layer build hard-fails without it. **Nothing from OpenVR is linked or
redistributed**: the layer loads SteamVR's own `openvr_api.dll` at run time,
located through `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`, and resolves its
entry points with `GetProcAddress`. The layer DLL therefore carries no import on
it and runs unchanged where SteamVR is absent — the feature is inert on every
other runtime.

OpenVR is BSD-3-Clause, and because the released binary is built from that
header, Valve's notice ships in `licenses/OpenVR-BSD-3-Clause.txt`. See
`THIRD_PARTY.md`.

The probe needs no D3D12, no FidelityFX and no layer, so it still builds on a
checkout that cannot build the layer:

```powershell
cmake -S . -B build-probe -G "Visual Studio 17 2022" -A x64 `
  -DXRFG_BUILD_LAYER=OFF -DXRFG_BUILD_STANDALONE=OFF -DXRFG_BUILD_TESTS=OFF
cmake --build build-probe --config Release --target xrfg_steamvr_delivery_probe
```

It is **not registered as a test**: it needs a live SteamVR session with an
application running, so it is run by hand, like `xrfg_nvidia_optical_flow_probe`.

It writes to `%LOCALAPPDATA%\OFXR Bridge\DeliveryProbe\` - beside the flight
logs rather than in them - one line per second, stamped with the wall clock.
The flight log's `ms=` is elapsed since session start and its filename carries
the wall clock of that start, so the two join offline on that column.
