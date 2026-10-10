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
| **OFXR, DLSS vectors + FidelityFX flow** (new) | both, each pixel from whichever explains the two frames better: the least error measured |
| **OFXR extrapolation** (new, SpaceWarp-style) | the newest frame moved on along the game's vectors and depth, shown after it: no added latency |
| **Native DLSS FG** (new) | NVIDIA's DLSS Frame Generation feature, fed the game's DLSS motion vectors and depth |

In native-enabled builds, it also makes OFXR + DLSS vectors work without a
separate guide provider. It fixes a hang under SteamVR, corrects the flight
recorder's GPU timings, and adds benchmark and quality tooling. The rest of
OFXR Bridge is unchanged from upstream: the tray, Prefer FPS over latency,
3X Frame Gen, Lower VRAM, the D3D11 and Vulkan bridges, SteamVR pacing, the
FPS overlay (now with a [status panel](#the-status-panel) beside it) and the
flight recorder. See [Using OFXR Bridge](#using-ofxr-bridge).

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
  OFXR's 3X Frame Gen setting. 3X needs NGX multi-frame generation (RTX 50
  series). On other adapters, native mode stays at 2X, so the game has to
  hold half the refresh rate rather than a third: on an RTX 40 card with a
  game that only just holds half, OFXR's own 3X is the better choice. The
  tray's 3X entry and the in-VR status panel say so.
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
  OFXRBenchmark.exe      (the tray's "Benchmark this PC")
licenses/
  NVIDIA-DLSS.txt, plus the SafetyHook, Zydis, Zycore and UEVR API notices
```

When you copy the tray build, keep `nvngx_dlssg.dll` (and `OFXRBenchmark.exe`,
which finds it there) with the other files in
`ofxr`, together with `licenses/NVIDIA-DLSS.txt`. When the tray arms, it
installs that DLL beside its cached layer. Do not distribute PDBs, static
libraries, test executables or the NVIDIA SDK.

### Turning it on

In the tray menu, under **Frame generation method**, select **NVIDIA DLSS
Frame Generation (experimental)** before the game starts its OpenXR session.
The choice applies to the next session. Select one of the optical-flow methods
in the same list to go back to OFXR's own. While native generation is
selected, the menu's status line and the tray tooltip name it. **3X** under
*Frames shown per game frame* switches native generation to 3X.

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

**Native generation's resolution.** The tray's **Quality and performance >
DLSS Frame Generation resolution** submenu offers 100%, 67% (the default)
and 50%, applied the next time the game starts. Below 100%, NVIDIA generates
at that fraction of each
eye's resolution, and the bridge puts back the real frames' detail wherever it
can follow the game's motion. NVIDIA's full-resolution frame is the softest of
the three: on 42 recorded Galactic Racer triplets, 67% erred less where the
scene moved (7.96 against 8.04), with better SSIM (0.786 against 0.771) and
more of the real frames' detail. It also costs about a quarter less: on a
Steam Frame (3004x3004 per eye, 120 Hz, racing) Galactic Racer rendered 106.3
game frames a second at 67%, against 99.5 at 100%; offline a turning-head 2X
pair costs 1.74 ms at 67%, against 2.35 ms.

50% is faster again (107.8 game frames a second there; offline 1.40 ms a
pair) and measured like 67% on the recorded frames (7.95), but a sharp
synthetic scene loses visible detail at its moving edges, and thin
structures shimmered live. If you want a cheaper method still, use OFXR + DLSS
vectors.

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

## Two new OFXR modes: a hybrid, and extrapolation

**DLSS vectors + FidelityFX flow.** In a game with DLSS, OFXR + DLSS vectors
follows the game's motion vectors exactly, but some content does not move the
way its vectors say: a shadow cast by something moving with the camera onto
ground rushing past, a reflection, an object that writes no velocity. Optical
flow follows the image instead, and loses elsewhere. The hybrid, the default
since release 18 (`[ofxr] dlss_flow_hybrid=1`), runs FidelityFX's flow as well and composes
each pixel from whichever explains both frames better, comparing their samples
blurred a little; where the vectors already explain both frames, the flow is
not asked. On 42 recorded Galactic Racer triplets it had the least error of
every method where the scene moved, with the best SSIM:

| Method (2X, recorded Galactic Racer frames) | Error where the scene moved | SSIM |
|---|---|---|
| **OFXR DLSS vectors + FidelityFX flow** | **7.18** | **0.815** |
| OFXR NVIDIA slow / medium flow | 7.59 / 7.67 | 0.792 / 0.791 |
| OFXR FidelityFX full / half-res flow | 7.83 / 7.86 | 0.782 / 0.779 |
| Native DLSS FG 85% / 67% / 50% / 100% | 7.86 / 7.96 / 7.95 / 8.04 | 0.786 / 0.786 / 0.788 / 0.771 |
| OFXR + DLSS vectors | 8.40 | 0.806 |
| A blend of the two frames | 12.99 | 0.607 |

OFXR's methods generate at the instant the frame times say; native generation
is fixed at a half, which costs it where frames come unevenly. Its flow, which
only patches what the vectors miss, runs at a quarter per axis: on the
recorded frames that erred 7.19 against 7.18 at half, for 11% less time. It
costs 1.03 ms a stereo pair offline at 3004x3004 per eye (1.58-1.71 ms live
in Galactic Racer on a Steam Frame with the half-resolution flow), against
1.81 ms for native generation at 67%. It takes the FidelityFX backend. With exact
synthetic vectors it errs more than the vectors alone at occlusions, where
their two samples must disagree and the flow can look the better explanation.

**Extrapolation, SpaceWarp-style.** Every interpolating method, NVIDIA's
included, holds each real frame back a display period, to show a frame
between it and the one before first. With `[ofxr] extrapolate=1`, OFXR shows
each real frame as soon as it is ready, at its own display time, and then
predicts the next display period from it, as Meta's Application SpaceWarp
does: a display period less latency, for the quality of a prediction. It
follows the game's DLSS vectors and depth where the game has them, and
FidelityFX's optical flow where it does not, as Meta's Asynchronous SpaceWarp
does.

- It draws Meta's warps. A grid over each view, a vertex every 16 pixels of
  the real frame, is moved on along the game's vectors (less the head's turn,
  which the runtime's own reprojection supplies) and depth-tested, as the
  Meta XR Simulator draws Application SpaceWarp: each vertex takes the motion
  and depth of the nearest of four taps around it, so a moving surface's edge
  moves with it, the nearest surface wins where the grid folds, and the grid
  stretches over what moving content uncovers. Without the game's vectors,
  FidelityFX's optical flow drives the same grid as Meta's Asynchronous
  SpaceWarp does, the smallest motion winning where it folds.
- Predicting a whole frame ahead from recorded frames, a harder test than the
  half frame a headset needs, it erred 12.0 where the scene moved, against
  20.6 for showing the last frame again, with SSIM 0.683 against 0.489. From
  FidelityFX's flow it erred 11.9, with SSIM 0.673: the flow follows a shadow
  the vectors move with the ground, but leaves more structure out of place.
  OFXR's own per-pixel gather, which it replaced, erred 12.6 and 12.4 (see
  [Other changes](#other-changes-in-this-fork)); `[ofxr] extrapolate_mesh=0`
  brings it back.
- **Measured, it shows each frame a display period sooner.** Live in Galactic
  Racer on a Steam Frame at 120 Hz, a real frame went down 45.5-47.0 ms after
  the game's xrWaitFrame returned when extrapolating, against 54.1-55.3 ms
  for native generation interpolating in the same session, and 62.5 ms for
  any interpolating method with the deeper pipeline on, as it is by default.
  The prediction shown after it is made for its own display time, so every
  frame on screen is that much fresher. (SteamVR's own lead from its wait to
  the display is 35.3 ms there; the rest is the game rendering.)
- It costs 0.16 ms a pair offline at 3004x3004 per eye and 0.17 ms live in
  Galactic Racer, cheaper than any interpolating method; native generation at
  67% took 1.59-1.63 ms in the same rounds. From FidelityFX's flow it costs
  0.48 ms offline and 0.78 ms live. In Hubris, a native OpenXR Unreal Engine 4
  game, the earlier gather ran through SteamVR at 0.29 ms a pair from the
  game's vectors and 1.12 ms from FidelityFX's flow, each real frame handed
  over before its prediction.
- Live at 3004x3004 per eye, with the mesh and the gather interleaved in one
  race (the warp is taken up from the INI at each tray control change), the
  median per pair was:

  | Extrapolation, live | Mesh | Gather |
  |---|---|---|
  | From the game's vectors | 170-172 us | 432-692 us |
  | From FidelityFX's flow | 777-782 us | 1541-1714 us |

  In the one set of rounds SteamVR held at 120 Hz, both meshes showed 118.5
  frames a second and the gathers 113.4 and 103.5; real frames went down
  45.7-46.2 ms after the wait in all four.
- `extrapolate=2` also runs FidelityFX's flow, at a quarter per axis: the
  vectors' grid is drawn as above, and wherever a pixel's point does not
  explain the frame before, the flow's prediction is asked for that pixel and
  the better kept. It erred least of all (11.56, SSIM 0.682) for 0.86 ms a
  pair on the recorded frames, against 0.17 ms from the vectors alone, so it
  is an INI option rather than a tray one.
- Content its vectors do not describe, such as a shadow on ground rushing
  past, moves with the vectors; there is no second frame to correct it.
  The deeper pipeline is turned off, since its held period would add the
  latency back.

| Where | Hybrid (default) | Extrapolation |
|---|---|---|
| The tray menu, **Frame generation method** | **Interpolate, DLSS vectors + FidelityFX flow** | **Extrapolate, SpaceWarp-style** |
| Tray settings (`tray.ini`) | `[tray] dlss_hybrid=1` (`0` for the vectors alone) | `[tray] extrapolate=1` |
| A directly loaded layer (`ofxr_bridge.ini`) | `[ofxr] dlss_flow_hybrid=1` (`0` for the vectors alone) | `[ofxr] extrapolate=1` |
| For tests, the game's environment | `XRFG_TEST_DLSS_FLOW_HYBRID=1` or `0` | `XRFG_TEST_EXTRAPOLATE=1` (or `2`) |

Both apply to OFXR's own algorithm. Extrapolation takes the FidelityFX
backend in every game. The hybrid needs the game's DLSS vectors: a session
starts on the engine chosen in the tray, and the first DLSS vectors the game
publishes switch it to the hybrid and FidelityFX, which in Galactic Racer
happened before generation began; a game without DLSS keeps the chosen
engine. Both are read when the game starts its OpenXR session, and
extrapolation wins if both are set. Tray settings saved before release 18
wrote `dlss_flow_hybrid=0` whatever was chosen, so the tray now keeps the
hybrid under a new key and ignores the old one.

## Other changes in this fork

- **Generated frames are made, and sent, from the head's pose when they are
  shown.** Reported in Galactic Racer under UEVR on SteamVR: "laggy black
  bars when moving my head". Every generated frame was made in the newer
  real frame's camera and handed to SteamVR with that frame's pose, although
  it is shown a display period earlier (two and one at 3X). The runtime turns
  every frame from the pose it was sent with to the head's pose when it is
  shown, so it turned each generated frame back by the head's motion over
  those periods, and the edge it turned away from had no pixels: black
  strips at the side of the view, flickering at the game's frame rate. At
  3X, 144 Hz and 120 degrees a second they were 1.7 and 0.8 degrees wide.
  Now each generated frame is made in, and sent with, the pose between the
  two real frames' at its own share of the way - the midpoint, or thirds at
  3X; extrapolating, carried on past the newer frame - so the runtime has
  nothing to turn back. What the newer frame did not see on the side the head
  turned from comes from the older frame, which did; extrapolating, the side
  the head turns towards was seen by neither and takes the nearest edge of
  the picture rather than black. Every method has it: FidelityFX and NVIDIA
  optical flow, DLSS vectors, the hybrid, extrapolation, 3X and native DLSS
  FG. It costs about 12 µs a generated frame at 2004x2004 per eye on an
  RTX 5090. In the tests, with the head turned 12 degrees between two real
  frames, each generated frame matched the scene seen from its own pose
  within 0.12-0.33 (in 255) across the whole view, with no black pixel; made
  the old way, 8-15% of the view was left without pixels once the runtime
  had turned it.

  `[ofxr] synthetic_pose=real` in `ofxr_bridge.ini` goes back to the old
  behaviour, for comparison; `interpolated` is the default. A running game
  follows an edit within half a second, and the tray keeps the line when it
  rewrites the file. With the flight recorder on, every frame of a pair
  gets a `reprojection_angle` record: how far the runtime will turn it to
  show it, in thousandths of a degree (`a`), beside the head's turn over a
  display period (`b`). While turning your head, a generated frame (`result=2`)
  should now read about what a real one (`result=1`) does, where with `real`
  it read about `b` (twice that for the first of the two at 3X). Details in
  [BUILDING.md](docs/BUILDING.md#each-synthetic-in-its-own-camera).
- **A status panel in the headset.** Turn a controller upside down, as with
  xrFPS, and a panel above it shows the method the tray asks for against the
  one running, why they differ when they do, and the frame rates, added
  latency and GPU time. See [The status panel](#the-status-panel).
- **Extrapolation draws Meta's mesh warps.** A grid moved along the game's
  vectors and depth (Application SpaceWarp) or along optical flow
  (Asynchronous SpaceWarp) replaced OFXR's per-pixel gather, on the 42
  recorded triplets predicting a whole frame ahead:

  | Extrapolation | Error | SSIM | Gradient error | GPU a pair, recorded frames |
  |---|---|---|---|---|
  | Gather, DLSS vectors and depth | 12.63 | 0.684 | 11.17 | 0.76 ms |
  | **Mesh, DLSS vectors and depth** | **12.00** | **0.683** | **10.89** | **0.17 ms** |
  | Gather, FidelityFX flow | 12.41 | 0.660 | 12.16 | 1.22 ms |
  | **Mesh, FidelityFX flow** | **11.90** | **0.673** | **11.49** | **0.47 ms** |
  | Gather, both (`extrapolate=2`) | 11.88 | 0.688 | 10.98 | 1.49 ms |
  | **Mesh, both, quarter-resolution flow** | **11.56** | **0.682** | **11.03** | **0.86 ms** |

  The mesh is a little softer (sharpness 0.85 against 0.90) where triangles
  stretch; the gather run over the mesh at motion edges, to sharpen them,
  erred more (12.40), so the mesh's own depth-tested edges are kept. Its
  cells were measured at 4, 8, 16, 24 and 32 pixels; 16 was the best.
- **The hybrid's flow runs at a quarter resolution.** Where the flow only
  patches what the game's vectors miss - the hybrid, and the combined
  extrapolation - a quarter per axis measured the same as half (7.19 against
  7.18; 11.56 against 11.53) for 11-16% less time. Alone the flow keeps its
  half: FidelityFX interpolation lost 0.27 at a quarter and its extrapolation
  0.71.

- **The game is released in the right phase.** With the deeper pipeline a
  real frame went down 62 ms after the game's wait in some stretches and 70
  ms in others, at the same 119.7 frames a second. The game's slack can be
  taken by the capacity wait before synthesis or by the pair hold after it,
  and both are stable; before synthesis, the finished frame waits out a
  period, rendered for an older pose. Galactic Racer settled that way after
  SteamVR's rate dipped, for 57% of a race's pairs. The layer now watches the
  wait before synthesis and, when nine in ten pairs of a window spend more
  than half a period there, holds the game one presenter frame longer once;
  live that left 1.7-6.8% of pairs there, each stretch over within a second,
  and every full-rate round at 62 ms. Triggered instead by a run of late
  pairs - eight in a row, the length that left the fewest pairs slow over
  three races' natural runs - each slip is moved within an eighth of a
  second: 1.0% of a race's full-rate pairs, no run longer than 9. While SteamVR runs at half the rate the
  promise stops measuring too, where it had swung between one period and
  three. Details in [BUILDING.md](docs/BUILDING.md).
- **Each synthetic goes at its share of the frame period.** The layer placed
  each synthetic by the display times the game stamped its frames with - a
  game below half the display rate had its synthetic at the instant its
  content was from. But frames do not go down at their stamps: a pair takes
  its two slots whatever the stamps say, and a frame that misses its slot
  repeats the one before. Against where every synthetic was actually shown
  between its real frames, live in Galactic Racer on the Steam Frame, the
  fixed share (the midpoint, thirds at 3X, 1.5 spans extrapolating) erred
  0.0080 on average where the stamps erred 0.0159, 0.0046 against 0.0089
  without the deeper pipeline, 0.0211 against 0.0311 at 3X and 0.0126
  against 0.0205 extrapolating; in Hubris 0.0010 against 0.0124. Details in
  [BUILDING.md](docs/BUILDING.md).
- **Each eye gets its own DLSS vectors in games with a swapchain per eye.**
  The game's DLSS vectors are matched to the image they belong to by size,
  and a game that submits each eye from its own swapchain - each the size of
  its eye's DLSS output - matched both eyes' evaluations. The newest went to
  both; with the capture at xrEndFrame (the default for D3D12 games), that
  is the eye DLSS ran last, every frame, so the other eye's generated frames
  followed its motion: a double image of the generated frames, in OFXR's
  vectors mode and native DLSS FG alike, but not in the DLSS vectors +
  FidelityFX hybrid, which falls back to flow where the vectors do not fit
  (reported on Virtual Desktop in MSFS 2024 and CONTROL Resonant). An
  evaluation written straight into the image is now that image's; otherwise
  each eye takes the newest evaluation published by the time its own image
  was released, as a game evaluates an eye and then releases it. Lies of P
  under UEVR is such a game - one DLSS feature evaluated for each eye in turn,
  each eye's image released just after - and live its vectors are now used for
  both eyes (status "used", none rejected after start-up) where the first fix,
  which went by the evaluations' count, gave a launch whose count began on
  the right eye the wrong eye or none. Galactic Racer and Hubris submit both
  eyes from one image and were never affected. The flight log names each
  per-eye swapchain's eye (`presenter_transition` 750) and records every DLSS
  evaluation (`dlss_evaluation`).
- **Tested in other Unreal Engine games.** On the Steam Frame, beside
  Galactic Racer and Hubris:

  | Game | How it runs | Placement error, fixed share / stamps | Per pair, live |
  |---|---|---|---|
  | Subnautica 2 | UE5, UEVR Native Stereo, ships DLSS-G | 0.0224 / 0.0406 and 0.0244 / 0.0417 (two launches) | vectors 0.38 ms, FidelityFX 1.02, native 67% 1.35-1.36, hybrid 1.47, native 100% 1.77, NVIDIA medium 8.5 - the same to 0.02 ms in both launches |
  | Lies of P | UE4, UEVR Alternating/AFR | 0.0299 / 0.0450 | vectors 0.36 ms, FidelityFX 0.87, hybrid 1.31 (no native: the game ships no DLSS-G) |
  | Kayak VR: Mirage | UE4, native OpenXR (`-hmd=OpenXRHMD -dx12`) | 0.0011 / 0.0253 | FidelityFX 1.03 ms, NVIDIA medium 5.7 (DLSS off in its menu) |
  | RoboQuest VR | UE5, native OpenXR, no DLSS | 0.0016 / 0.0022 | FidelityFX 2.0-2.2 ms, NVIDIA medium 12 |

  Run UEVR games in **Native Stereo** where they allow it: one double-wide
  image, the shape every method was tuned on. Alternating/AFR gives each eye
  images of its own; it generates too since release 16 (Lies of P), but it
  costs more of SteamVR's sixteen swapchains. Bootstrap Island uses OpenVR and
  is outside an OpenXR layer's reach; Hogwarts Legacy exits while this UEVR
  build hooks D3D12, before any OpenXR session.
- **Tested across a Steam library.** On the Steam Frame at 120 Hz (2X, the
  hybrid by default), shown frames a second as real + generated:

  | Game | How it runs | Shown a second | Per pair, live |
  |---|---|---|---|
  | Riven | UE5, native OpenXR (`-vr`), DLSS | 59.8 + 59.8 | hybrid 1.13 ms (taken up 0.06 s after the first DLSS) |
  | Firmament | UE, native OpenXR (`-vr`), DLSS | 59.7 + 59.6 | hybrid 1.12 ms |
  | Halo: Campaign Evolved | UE5.6, UEVR Native Stereo | 60 + 60 (112 real a second without) | NVIDIA medium 6.0 ms, FidelityFX 1.2 (DLSS not on from its menu) |
  | Surreal 98 | UE5.8, native OpenXR (`-vr`), D3D12 | 33-37 + the same | NVIDIA medium 3.7 ms, FidelityFX 0.93 (the game holds about 38 alone) |
  | The Midnight Walk | UE5, native OpenXR (`-vr XRSystem`) | 59.9 + 59.9 | NVIDIA medium 3.5 ms (no DLSS in VR) |
  | Out of Sight VR | UE5, native OpenXR | 59.7 + 59.7 | NVIDIA medium 3.4 ms |
  | Moss: Book II | UE4, native (`-hmd=OpenXRHMD`; it tries Oculus first) | 59.6 + 59.7 | NVIDIA medium 4.0 ms |
  | Legendary Tales | UE, native OpenXR | 60 + 60 | NVIDIA medium 3.4 ms |
  | FlatOut 4: Total Insanity VR | D3D11 (the D3D11 bridge) | 59.8 + 59.8 | NVIDIA medium 2.8 ms |
  | Gunman Contracts | Unity, D3D11 (bridge) | 59.8 + 59.9 | NVIDIA medium 3.6 ms |
  | Little Nightmares VR | Unity, OpenXR | 59.7 + 59.7 | NVIDIA medium 7.2-7.8 ms |
  | theBlu | Unity, D3D11 (bridge) | 59.8 + 59.8 | NVIDIA medium 1.8 ms |
  | Escape Simulator | Unity (`es_vr`) | 59.8 + 59.7 | NVIDIA medium 3.7 ms |
  | WHAT THE BAT? | Unity, D3D11 (bridge) | 56.3 + 56.2 | NVIDIA medium 3.7 ms |
  | Trombone Champ: Unflattened | Unity, D3D11 (bridge), 6514x6514 per eye | 59.7 + 59.7 | 3.8-4.7 ms since the flow's input is capped (was 19-21 ms) |
  | Beat Saber | Unity, D3D11 (bridge) | 60 + 60 | NVIDIA medium 4.0 ms (needed the invalid-redist fallback) |
  | Drop Dead: The Cabin | Unity | 31 + 31 | NVIDIA medium 1.9 ms (the game holds about 31) |

  Unity games ask for OpenXR 1.1 first, which SteamVR refuses, and then make
  their instance at 1.0; FlatOut does the same. Not generating, and not
  because of the layer: The Riese Project - Prologue waits on a launcher that
  needs a mouse click; this Bulletstorm VR install (with UE4SS mods) never
  starts an OpenXR session; the Stellar Blade demo crashes in its own code
  under this UEVR build with or without the layer.
- **Extrapolation goes on by the game's own step.** The DLSS vectors span the
  game's step from the previous frame - the frame time it gives DLSS - and a
  frame extrapolated a display period on should show the scene a display
  period of game time on: 1 + period / step, the fixed 1.5 only when the step
  is the frame period. Galactic Racer's steps swing about the cadence (12-20
  ms in one race at 60 frames a second) and back the next frame. Scored
  against the game's own steps - the next frame's step spread over the
  display until it is shown, which is what makes the shown motion even - the
  layer's extrapolation erred 0.0596 on average where the fixed share erred
  0.1103 and the stamps 0.1119 (0.0179 against 0.0266 in a steadier race),
  and in Subnautica 2 0.0313 against 0.0365 and 0.0516.
  Against display times alone, which assume the game steps evenly, the fixed
  share stays closer (0.0050 against 0.0601); that assumption is what the
  game's steps break. A game that gives DLSS no frame time, or a step outside
  half to twice the frame period, keeps the fixed share. The flight log's
  `dlss_evaluation` records carry the frame time.
- **Only the images a projection uses are armed.** Any change from the tray -
  and the first one, at the start of every session - armed every swapchain
  the game had made that was still eligible, not only the ones a projection
  layer had named. Under UEVR that gave three of SteamVR's sixteen swapchains
  to each 2560x1440 spectator or UI image before the eyes were named, and in
  Lies of P and Subnautica 2 under Alternating/AFR the eyes were then refused
  (XR_ERROR_LIMIT_REACHED) and the session passed through without
  generating - on some launches and not others, by whether the tray's first
  change came before the first projection. The tray now arms only images a
  projection has named; the projection path arms the rest when named. Two
  Lies of P launches in a row then armed the eyes alone and used both eyes'
  vectors, and Galactic Racer armed its one projection image, not its three
  1707x869 ones.
- **The game is promised the time its frame is shown.** The display time a
  game is handed at xrWaitFrame assumed its frame would go down within a
  display period; a game rendering at half the display rate takes most of
  two. Measured in Galactic Racer, each real frame went down a period after
  its promised time extrapolating, and two periods after interpolating with
  the deeper pipeline: the game rendered every frame for a head pose that much
  early, and SteamVR's reprojection made up the difference. The layer now
  counts, at the presenter, how many whole periods late real frames go down,
  and moves the promise to the window's median (`[ofxr]
  promise_shown_time=1`, the default) once two 64-frame windows in a row name
  it. Live, real frames then went down at their promised time in every mode.
  When frames go down is unchanged; only the promise follows it.
  `xrfg_layer_promise_shown_time` checks it on the presenter, interpolating
  and extrapolating.
- **The promise follows games whose frames go down in two groups.** It moved
  only when nine in ten of a window agreed. Kayak VR's real frames go down in
  two groups a frame apart (about 72% and 27%, as Unreal Engine 4 ends a frame
  now before and now after its next wait), so it never moved, and with
  FidelityFX flow 71% of its frames went down a period after their promise -
  the game rendered them for a head pose that much early. Following the
  median, 73% now go down at their promise and the rest a frame later, in
  every method. `xrfg_layer_promise_two_groups` hands every fourth frame over
  late on the fake runtime and fails with the old rule.
- **Extrapolation from the game's vectors reads depth again.** In release 6,
  the combined mode's packed constants also reached the vectors-only shader,
  which then read the depth rectangle from the wrong constants and scored
  every pixel for a choice it never made: it erred 13.83 where the scene
  moved on the recorded frames instead of 12.68. Both are fixed.
- **Extrapolation searches five candidates, not nine.** Near a motion edge
  each pixel solved from its own motion and its neighbours' at 12 and 40
  pixels. The outer ring erred more on the recorded frames, and a headset
  predicts half as far as that test. With the pixel and its four nearest
  neighbours: from the game's vectors 12.63 error and SSIM 0.684 against
  12.68 and 0.679, for 0.76 ms a pair warm against 1.13; from both, 11.88 and
  0.688 against 11.87 and 0.684, for 1.49 ms against 1.93. FidelityFX's flow
  is measured in blocks, so its motion steps between them without an edge;
  with a 6-pixel step threshold too and its neighbours 8 pixels off, flow
  extrapolation erred 12.41 with SSIM 0.660 against 12.46 and 0.652, for
  1.22 ms against 1.94.
- **Latency in the flight log.** Each submission records the display time
  the game was promised for the newest real frame it holds and the one it
  went down for (`presenter_content`), with the log's clock origin
  (`clock_origin`), so latency is read from a log rather than inferred.

- **Optical flow keeps pixels the camera alone does not explain.** Where the
  flow's two samples disagreed, the optical-flow methods fell back to a blend
  that follows only the headset's turn, which in a game that moves is a double
  image. Each pixel now takes whichever explains both frames better, the flow
  or the camera alone, and the flow keeps a tie. NVIDIA's cost and the fast
  preset's endpoint check, which both leaned on the blend, no longer weight
  it. On the recorded triplets FidelityFX flow's error fell from 8.21 to 7.86,
  NVIDIA medium's from 8.17 to 7.67 and NVIDIA fast's from 9.01 to 7.94; a pure
  head turn now errs 0.08 against 0.16.
- **OFXR + DLSS vectors treats any non-uniform motion as two surfaces.** Where
  its two samples disagree, B's warped sample stands in wherever a vector 16
  pixels away differs by a tenth of a pixel, not a whole one: ground rushing
  past gains from it too (error 8.49 to 8.40). A fade over uniform motion
  still blends.
- **NGX's linearised depth is scaled by a tenth**, as NVIDIA suggests for
  compressed depth: native generation's error fell by 0.12 at 100% and 0.09 at
  67%. NGX's other depth heuristics, pre-dilated vectors, per-eye features and
  the seam width measured no better than they are.

- **DLSS guides matched to the right eye.** When each eye has its own DLSS
  feature, as under UEVR, the guides were matched to eyes in the order their
  streams first appeared. Whenever the right eye evaluated first, each eye
  got the other eye's motion and depth. In Galactic Racer that was so on
  every recording, and replaying those frames with the eyes put right took
  OFXR + DLSS vectors' error where the scene moved from 9.6 to 3.5, and native
  generation's from 4.8 to 3.6. Eyes are now ordered by where their inputs
  sit in the shared render target, the left eye's on the left, and by first
  appearance only when that cannot tell them apart.
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
| OFXR + DLSS vectors | 0.13 ms | 0.23 ms | 0.52 ms | 0.72 ms |
| OFXR FidelityFX, half-res flow | 0.20 ms | 0.31 ms | 0.58 ms | 0.78 ms |
| OFXR FidelityFX, full-res flow | 0.38 ms | 0.64 ms | 1.30 ms | 1.80 ms |
| Native DLSS FG 2X | 1.14 ms | 1.49 ms | 2.38 ms | 3.42 ms |
| OFXR NVIDIA medium flow | 1.30 ms | 1.61 ms | 2.89 ms | 3.80 ms |
| Native DLSS FG 3X | 1.66 ms | 2.13 ms | 3.73 ms | 5.02 ms |

At 3004x3004, OFXR's DLSS vectors + FidelityFX flow costs 1.61 ms a pair and
extrapolation 0.43 ms (0.53 ms for interpolating from the vectors).

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
- The two new modes, live on the same track, interleaved with native
  generation at 67% (1.61-1.82 ms): DLSS vectors + FidelityFX flow
  1.58-1.71 ms, and extrapolation 0.66-0.72 ms from the game's vectors
  (0.71-1.11 ms before its five-candidate search) and 1.11-1.76 ms from
  FidelityFX's flow.

**Latency, live.** Galactic Racer on a Steam Frame at 120 Hz, from the game's
xrWaitFrame returning to its real frame going down, read from the flight log
(`presenter_content` against SteamVR's display times; SteamVR's own wait leads
its display by 35.3 ms). Rounds where SteamVR halved the rate are left out.

| Mode | Real frame shown after the wait |
|---|---|
| OFXR extrapolation, DLSS vectors / FidelityFX flow / both | 45.5-46.9 / 45.4-47.0 / 45.8 ms |
| Native DLSS FG, deeper pipeline off (an extrapolating session) | 54.1-55.3 ms |
| OFXR interpolation, deeper pipeline on (the default) | 62.5 ms |
| Native DLSS FG, deeper pipeline on | 62.5-70.1 ms |

An interpolated frame is shown a period before the real frame it is made
from, and is a midpoint, so it shows content as old as that real frame: every
frame of an interpolating mode is that late.

**Offline, every mode, through the tray's benchmark.** `OFXRBenchmark.exe`
with the game closed, at 3004x3004 per eye and 120 Hz; all 37 cases ran, and
the reference timed before and after agreed within 0.2% (drift 1.002).

| Mode | Per pair | Mode | Per pair |
|---|---|---|---|
| OFXR + DLSS vectors | 0.50 ms | Extrapolation, DLSS vectors | 0.16 ms |
| FidelityFX 50 / 75 / 100% | 0.58 / 0.88 / 1.31 ms | Extrapolation, FidelityFX 50 / 75 / 100% | 0.48 / 0.77 / 1.20 ms |
| FidelityFX 3X | 0.81 ms | Hybrid 50 (quarter flow) / 75 / 100% | 1.03 / 1.49 / 1.92 ms |
| Native 100 / 67 / 50% | 2.58 / 1.81 / 1.47 ms | Native 3X 100 / 67 / 50% | 3.78 / 2.88 / 2.44 ms |
| NVIDIA fast / medium / slow 50% | 2.47 / 2.89 / 4.41 ms | NVIDIA medium 50%, both ways | 4.49 ms |
| Guide snapshot (game side) | 0.03 ms | | |

The extrapolation and hybrid rows were timed again on the mesh and the
quarter-resolution flow (drift 1.000).

**Every interpolating method live, on the final build.** Galactic Racer at
2316x2316 per eye on a Steam Frame (120 Hz, the deeper pipeline on), the
methods switched in turn through the layer's control in two races; only
rounds SteamVR held at 120 Hz are counted. The order matches the offline
table, and native's and NVIDIA's flow cost more beside a running game:

| Method | GPU a pair, live | Frames shown a second | Real frame after the wait |
|---|---|---|---|
| OFXR + DLSS vectors | 0.37-0.38 ms | 119.6 | 62.5 ms |
| FidelityFX 50% | 0.84-1.03 ms | 119.5-119.6 | 62.5 ms |
| DLSS vectors + FidelityFX flow | 1.32-1.44 ms | 119.6 | 62.4-62.6 ms |
| Native 67% | 1.38-1.69 ms | 113.9-119.7 | 62.5-62.6 ms |
| Native 100% | 2.52 ms | 114.5 | 62.7 ms |
| NVIDIA medium 50% | 4.71-5.62 ms | 88.6-108.7 | 62.7-70.9 ms |

NVIDIA's flow was the one method that cost frames there, and the rounds
after it were the ones SteamVR dropped to half the rate.

Again on release 14 (the cadence placement and the per-eye vectors), one
race, every method in turn: OFXR + DLSS vectors 0.38 ms, FidelityFX 0.83-0.85
ms, the hybrid 1.47-1.70 ms, native 67% 1.64-2.21 ms, native 100% 2.15 ms and
NVIDIA medium 5.35 ms a pair, the real frame 61.8-62.5 ms after the wait in
every 120 Hz round - the same order and the same latency.

At 2886x2886 per eye the GPU was saturated (110-114 frames a second for
every method) and the spans were steady from round to round: OFXR + DLSS
vectors 0.57-0.58 ms, FidelityFX 1.24 ms, native 67% 1.54-1.61 ms, the hybrid
1.90-1.92 ms, native 100% 1.9-4.7 ms and NVIDIA medium 9.1 ms. Beside a game
that fills the GPU, FidelityFX's many short passes stretch to twice their
offline time while native's evaluation does not, so there native 67% takes
less of the GPU's time than the hybrid; the hybrid still errs least.

**"Prefer FPS over latency", live.** The tray's option (the deeper pipeline,
on by default) holds each synthetic a display period so synthesis has time
to finish. Galactic Racer at 3004x3004 per eye on a Steam Frame, where the
game only just reaches half the display rate, in two sessions each way. At
that load SteamVR halved the rate in most rounds either way; across the
rounds it held 120 Hz, the option on showed 110.6 frames a second against
103.3 off (3 rounds against 11), and each real frame went down 35-43 ms
after the game's wait against 17-27 ms (beyond SteamVR's own lead). It
trades about 7% more frames shown for 17-24 ms of latency where the game is
at its limit, and only latency where it is not; it stays on by default, as
upstream ships it. Extrapolation turns it off and shows each frame sooner
still.

An automatic depth was tried as well: deep, with the shallow pipeline tried
every so often and kept while it repeated no more frames. Over whole races at
2316x2316 per eye, two each way, the option on showed 102.6 and 104.3 frames
a second and the automatic depth 99.2 and 90.1 (54 ms against 62 where it ran
shallow), because the shallow stretches dropped frames and dropped frames are
what make SteamVR halve the rate. It was removed; the tray's choice is the
same trade. Those races also found the option costing a second period at
times; see "The game is released in the right phase" under
[Other changes](#other-changes-in-this-fork).

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
  of both frames at the same pixel wherever the two warped frames disagreed:
  both frames' edges at once, and ghosting through fades. Now:
  - Away from a motion edge, a disagreement is one surface whose shading
    changed, such as a fade or a flash, and the motion-compensated blend is
    shown.
  - Near a motion edge, background being uncovered comes from the new frame,
    background being covered comes from the previous one, and a short search
    finds the surface in front.
  - Content that stays put on screen although its vectors say it moved, such
    as a HUD drawn after DLSS, is kept.

  On the benchmark scene of a striped square sliding over a detailed
  background, the error fell from 3.95 to 1.74, and at the square's edges
  from 25.9 to 7.55. Native generation measures 4.25 and 8.7 there. At 3X,
  the frames a third and two thirds of the way err 1.38 and 1.37 (native:
  3.38 and 2.99; before: 4.88 and 2.96). A moving scene that darkens errs
  0.08, against 45.7 before. With the game's vectors exact, OFXR + DLSS
  vectors is now the more accurate method, at about a sixth of the GPU time.
  On recorded Galactic Racer frames, though, native generation had the least
  error (see [Testing](#testing)): the game's vectors do not describe every
  surface. In Hubris a pair rose from 0.32 to 0.37 ms, nearly all of it for
  the HUD check. On the offline benchmark, whose frames disagree almost
  everywhere, it rose about 15%.

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
- **The new frame's warped sample for the optical-flow methods too.** It
  took FidelityFX's edge error from 45 to 34 on the scene below. But
  estimated flow is unreliable where it would act, and the same-pixel blend,
  which follows the camera, is already exact for a static world: a rotating
  cropped view erred 4.4 to 4.8, against a limit of 1.0, even when gated on a
  4-pixel jump in the flow. The optical-flow methods keep the blend.
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
| `XRFG_TEST_FG_BENCH=1` | Every frame-generation method through the synthesizer, at 2004x2004 per eye by default: OFXR FidelityFX and NVIDIA flow, DLSS vectors, the hybrid, extrapolation, and native 2X/3X. It also times the game-side guide snapshot copies. The native rows need a native build. |
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

**Real game frames.** `XRFG_TEST_CAPTURE_FRAMES=<folder>`, set in the game's
environment, makes the layer record runs of consecutive frames: colour, views,
DLSS guides and the frame the headset was sent. `XRFG_TEST_CAPTURE_WAIT=1`
starts them when a file named `go` appears in the folder.
`XRFG_TEST_REPLAY=<folder>/seq<N>` then generates the middle frame of three
from the other two with every method, the game's vectors scaled by the
recorded frame times, and compares each with the real middle frame: the
absolute error overall and where the scene moved, and there SSIM over 8x8
blocks, sharpness (the output's gradient against the truth's) and gradient
error. The absolute error alone favours a blur.

| Variable | Effect |
|---|---|
| `XRFG_TEST_REPLAY_FIRST` | The first frame of the three. |
| `XRFG_TEST_REPLAY_FLOWS` | OFXR configurations to run instead, `backend/preset/scale[/bi]` separated by commas: `vectors`, `hybrid`, `extrapolate` (the game's vectors and depth), `extrapolate_flow`, `extrapolate_hybrid` (both), each through the gather, or `extrapolate_mesh`, `extrapolate_flow_mesh`, `extrapolate_hybrid_mesh` through Meta's mesh warps; `ffx` or `nvidia`; `slow`, `medium` or `fast`; `full`, `three_quarter`, `half` or `quarter`. |
| `XRFG_TEST_REPLAY_TIMING_PAIRS=n` | After scoring, runs each method's prime and pair n times more and prints their median GPU time (`warm_gpu_us`): the cost on real content. The one pair it scores is cold. |
| `XRFG_TEST_REPLAY_NATIVE_SCALES` | Native resolutions to run, for example `100,67`. `XRFG_TEST_REPLAY_NATIVE_ONLY=1` skips OFXR. |
| `XRFG_TEST_REPLAY_EXTRAPOLATE=1` | Predict frame 2 from frames 0 and 1 instead. |
| `XRFG_TEST_REPLAY_FRACTION` | Moves OFXR's frame, for example to native generation's half. |
| `XRFG_TEST_REPLAY_SAVE`, `_SAVE_FULL` | Write crops, or whole images, as PPM. |
| `XRFG_TEST_REPLAY_VECTOR_GAIN`, `_GUIDE_SHIFT`, `_SWAP_EYES` | Test the guides. |

The method table above comes from 42 triplets: 22 from Galactic Racer's Jakku
time trial and 20 from its Tatooine podrace, on a Steam Frame at 3004x3004 per
eye. Section by section the order changes: along a fast wall the game's
vectors did not describe the motion at any scale, and around the Tatooine
pod's shadow they moved it with the ground, while in a crash with debris they
beat every flow. The hybrid takes the better of the two per pixel. The full
sweep, every native setting and every flow option, is in docs/BUILDING.md.

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
- **Fixed cadence.** Native mode uses NGX's fixed fractions, as OFXR's own
  algorithm now does too: measured against where each generated image was
  actually shown, the fixed share beats any placement from the game's stamps
  (see "Each synthetic goes at its share of the frame period" above).
- **Camera translation.** Full reprojection of camera translation, and
  separate guides for baked-in HUD and UI, remain future work. The bridge's
  existing limitation for camera translation still applies.
- **Live testing so far** covers Galactic Racer under UEVR (two tracks), on a
  Steam Frame through SteamVR and on the Meta XR Simulator, and Hubris, a
  native Unreal Engine 4 VR game, through SteamVR.

## Using OFXR Bridge

For everyday use:

1. Run `OFXRBridgeTray.exe`. The bridge arms itself straight away.
2. Right-click the tray icon to choose the method and options.
3. Start the game normally. For injectors such as UEVR, start the tray before
   the game.
4. Select **Disarm bridge**, or close the tray, when you are finished.

### The tray menu

The menu is grouped in the order you decide things. Its top lines say whether
the bridge is armed or paused and which method is in use.

| Entry | What it holds |
|---|---|
| **Disarm bridge** / **Pause frame generation** | Arming, and the pause, with its key shown beside it. |
| **Frame generation method** | One list of methods: FidelityFX optical flow, NVIDIA optical flow (fast, medium, slow) or NVIDIA DLSS Frame Generation. Then how OFXR makes frames: **interpolate** (from the game's DLSS vectors where it has them, the chosen flow elsewhere; the cheapest), **interpolate with DLSS vectors + FidelityFX flow** (default, best quality; games without DLSS keep the chosen flow), or **extrapolate, SpaceWarp-style** (no added latency, less accurate; FidelityFX flow in every game, whatever engine is chosen above). Then **2X** or **3X**. |
| **Quality and performance** | Optical-flow resolution, NVIDIA's both-ways check, DLSS Frame Generation resolution, and Prefer FPS over latency. |
| **Benchmark this PC...** | See below. |
| **FPS overlay**, **Diagnostics**, **Advanced** | Overlay position and the status panel (below); flight recorder and logs; Lower VRAM and the pause key. |

Options that do not apply to the chosen method are greyed out. The settings
are stored as before in `%LOCALAPPDATA%\OFXR Bridge\tray.ini`; how OFXR makes
frames adds `dlss_hybrid=0|1` and `extrapolate=0|1`, which the tray also
writes to the layer's `[ofxr]` section as `dlss_flow_hybrid` and `extrapolate`.

### The status panel

In a game, turn either controller upside down and hold it there for a
quarter of a second: a panel appears above it, facing you, and says what
frame generation is doing right now. Turn the controller back and the panel
goes half a second later. It is the gesture from
[xrFPS](https://github.com/elliotttate/xrFPS), with the same angle
(more than 120 degrees from upright) and the same delays, and it follows the
controller while it shows. The FPS counter is hidden while the panel is up,
since the panel shows the same number.

The top right corner says how things stand:

- **GENERATING** (green): what the session was started with is running.
- **FALLBACK** (amber): frames are generated, but not the way the session was
  set up to make them. The **Why** lines say why.
- **NOT GENERATING** (red): frames pass through without generation, with the
  reason.
- **PAUSED** or **OFF**: paused from the tray, generation switched off, or the
  runtime out of swapchains for this session.

Under it, four numbers: **shown/s**, the distinct frames the headset gets a
second, the number the corner counter shows (green at the refresh rate, amber
within 15% of it, red below that); **game/s**, the game's own frames;
**generated/s**, the frames the bridge made; and the display's refresh rate.
Then a line each:

| Line | What it says |
|---|---|
| **Asked** | The method the tray is set to, with 2X or 3X and "deep" for Prefer FPS over latency. |
| **Running** | What the session runs now, after every fallback. For example, the DLSS vectors + FidelityFX hybrid runs the chosen optical flow until the game has given its first DLSS vectors, and the flow on its own in a game without DLSS. |
| **Why** | One line per reason, if there is any. Amber and red ones are fallbacks: no DLSS vectors from the game, vectors that did not match the frames, NVIDIA optical flow not available on this GPU, DLSS Frame Generation not available or waiting for the game's depth, 3X or the deep pipeline refused by the runtime's swapchain limit, a short hold after a change, eye images that could not be set up. Grey ones are not: the tray's method, the hybrid and extrapolation take effect when the game next starts, so a change made during the game shows here as "starts with the next game start", and 3X switches within a moment. |
| **Vectors** (OFXR) or **Guides** (DLSS Frame Generation) | The share of the last quarter second's frame pairs that used the game's DLSS vectors, or why none did: none from this game yet, refused because they did not match the frames, waiting for the game's depth. |
| **Pipeline** | Deep or shallow, and whether the bridge's own presenter thread hands the frames to the runtime or the game's thread does. |
| **Latency** | The display frames frame generation adds, in milliseconds at the refresh rate: interpolation shows each real frame a frame later (two at 3X), the deep pipeline one more, extrapolation none. "promise +1" means the game is told its frames are shown a frame later than planned, because that is when they go out. |
| **GPU** | Frame generation's GPU time for each pair of frames, averaged over about a second. It is measured only while the flight recorder is on. |
| **Frames** | Repeats a second: a frame handed to the headset again because the game had nothing new. On SteamVR also what the headset received a second. |
| **Session** | The game's graphics API, through which of the bridge's paths, and the OpenXR runtime. |

**FPS overlay** in the tray has three settings for the panel: **On a
controller turned upside down** (the default), **Always, low in the view**,
and **Off**. In the layer's ini they are `[overlay] panel=gesture`, `always`
and `off`.

To read the controllers, the bridge adds one action of its own to the game's
input: a controller pose, bound to both grips beside the game's own bindings.
If the runtime refuses it for a controller (a gamepad, for example), the
game's bindings are set up exactly as the game asked, so the game's input
never depends on it. A runtime's controller-binding screen, SteamVR's for
one, may list it for each game as "OFXR Bridge status panel". This is
decided when the game starts: a game
started with the panel on Always or Off needs a restart for the gesture, and
Always and Off leave the game's input untouched. A game that sets up no
controller input at all has no gesture; use Always there. If xrFPS is
installed as well, turning a controller over shows both. Disarming the bridge
removes the panel along with the counter.

The panel is repainted four times a second while it shows, on the game's
render thread, and costs nothing while it is hidden but two pose reads a
frame.

### Benchmark this PC

**Benchmark this PC...** measures, on your own graphics card, the GPU time of
every frame-generation method at your headset's per-eye resolution: each
optical-flow engine at each resolution, the modes on the game's DLSS vectors,
extrapolation from vectors and from flow, native DLSS Frame Generation at
100/67/50% in 2X and 3X, and the per-frame copy of a DLSS game's vectors and
depth. It takes about half a minute; close VR games first. At the end it
measures its first method again, and warns if the two differ by more than a
quarter, which means something else used the graphics card during the run.
The headset's resolution and refresh rate are filled in from the last session
the flight recorder recorded, or you pick a headset or type them. Methods the
PC cannot run (NVIDIA optical flow without an NVIDIA card, DLSS Frame
Generation without an RTX 40/50 card or `nvngx_dlssg.dll`, 3X without RTX 50)
are listed as not available, with the reason.

The run is `ofxr\OFXRBenchmark.exe`, started as a separate process, so the
tray stays responsive and keeps working while it runs. The results are kept
in `%LOCALAPPDATA%\OFXR Bridge\benchmark.ini` with the GPU, driver and
resolution. From then on each choice in the menu shows what it costs on this
PC and the best it can do, for example "0.59 ms, up to +93%"; results from
a different graphics card are not used.

How to read the numbers. With 2X, the headset shows two frames for every frame
the game renders, so at 90 Hz the game has 22.2 ms per frame instead of 11.1.
Frame generation runs on the same GPU, and its time comes out of that budget.
A game therefore reaches the full refresh rate when its own GPU time is at most
the budget minus the cost; the window shows that as the frame rate the game
must reach by itself, without frame generation ("Game needs"). The speed-up
is what a game running at exactly that rate gains: at R Hz with N frames per
game frame it is N - 1 - cost x R. Faster games gain less, since nothing goes
past the refresh rate, and slower ones cannot hold it with that method. 3X
works the same with three frames per game frame. The window also lists each
method's measured image error on recorded Galactic Racer frames, and the
latency it adds: interpolation shows each real frame one display frame later
(two at 3X), extrapolation adds none. The times are measured on test frames
with nothing else running; in a game, which shares the GPU, expect somewhat
more.

> [!IMPORTANT]
> **OpenXR games only.** Games built on OpenVR, SteamVR's older system, are
> not supported, even though they run on SteamVR. For UEVR, select its OpenXR
> option. Also, **never run the game as administrator.** The OpenXR loader
> ignores per-user layers in an elevated program, so the bridge never loads
> there.

The [upstream README](https://github.com/djules75/OFXR-Bridge#readme) has the
full user guide and explains every tray option; this fork's menu groups them
as above. For more, see:

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
