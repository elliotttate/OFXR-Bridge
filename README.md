# OFXR Bridge

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
only (see below).

Technically, OFXR Bridge is an experimental OpenXR API layer. It uses
optical flow to generate frames between the ones the game renders.

The working **V439 experimental build** also offers **NVIDIA DLSS Frame
Generation** in the tray. It runs the native NVIDIA NGX feature with separate
eye histories, engine motion and depth, alongside the existing OFXR option.
It can capture guides directly from a D3D12 DLSS or Ray Reconstruction upscaler,
or accept a cooperating producer through the V2 guide API. UEVR's public
projection API supplies the depth convention when OpenXR depth metadata is
absent. It needs an NGX-supported GPU/driver. Missing or reset guides show the
current frame. GPU regressions cover array and side-by-side stereo on an RTX
5090; live Galactic Racer tests on the Steam Frame verify guide capture and
native generation. Headset image quality and comparative performance remain
under evaluation. See
[building the native option](docs/BUILDING.md#native-nvidia-dlss-frame-generation).

Current release: **v0.2.13.1 (internal build V438)**.
See the [release notes](docs/releases/0.2.13.1.md). 0.2.13.1 is a
performance and stability release: it fixes the higher latency and the
frame drops some games had on Virtual Desktop since 0.2.12.1, stops
hangars, menus and loading screens dropping to 60 fps after a hiccup,
shortens the frame-rate dips SteamVR showed after a hiccup, stops MSFS
2024 staying "loaded but not generating" after one rejected frame, and
adds a tray switch for the VRAM saving of 0.2.10.1, for the cards on which
it costs smoothness.
0.2.12.2 removed a flicker in the left eye in MSFS 2024 with 3X Frame Gen,
and 0.2.12.1 lets you pause and resume frame generation while you play,
from the tray or with a key you choose.

> [!WARNING]
> **Pimax headsets: use SteamVR, not Pimax OpenXR.** Pimax Play's OpenXR
> runtime keeps an extra copy in VRAM of every image it is handed, so OFXR
> costs about **1.5 times** more on it than on SteamVR, and the copies stay
> until the game closes. We have reported it to Pimax so they can fix their
> runtime; there is nothing OFXR can do about it in the meantime. Until it is
> fixed, run your Pimax through SteamVR with
> [sboys3's native SteamVR driver](https://store.pimax.com/blogs/blogs/sboys3-native-steamvr-driver-setup-guide)
> and make SteamVR the active OpenXR runtime (SteamVR → Settings → OpenXR).
> This matters most in MSFS 2024, which is already close to the VRAM limit
> at Pimax resolutions. Details in the
> [troubleshooting guide](docs/TROUBLESHOOTING.md#how-much-vram-ofxr-uses).

## 🛠️ Not working, or not feeling smoother?

### ➡️ [Read the troubleshooting guide first](docs/TROUBLESHOOTING.md)

It checks in one step whether OFXR is running in your game, then lists the
usual causes and fixes: OpenVR games, running as administrator, the game's
frame rate, frame smoothing in your VR software, and running out of VRAM.

---

> [!IMPORTANT]
> **OpenXR games only.** The bridge works only with games that talk to your
> headset through **OpenXR**. Games built on **OpenVR**, SteamVR's older
> system, are not supported, even though they run on SteamVR: the bridge
> never loads in them and changes nothing.
>
> It doesn't matter which headset you have or which VR software it runs
> through (SteamVR, Pimax, Virtual Desktop): what matters is whether the game
> itself uses OpenXR. Examples of OpenXR games: Microsoft Flight Simulator
> 2020 and 2024, DCS World, Assetto Corsa EVO, and Unreal games played through
> **UEVR with its OpenXR option selected** (not OpenVR). Some OpenVR-only
> games can be run through OpenXR with
> [OpenComposite](https://gitlab.com/znixian/OpenOVR), for example Elite
> Dangerous, Skyrim VR and No Man's Sky.

> [!WARNING]
> This is experimental software. It may not work with your game, VR mod, GPU or OpenXR
> runtime. It may produce visual artifacts, fail to activate, freeze the game
> or cause a crash. Use it at your own risk.

> [!IMPORTANT]
> The bridge runs on any Direct3D 12 GPU with Shader Model 6 support - GTX
> 10-series and newer NVIDIA cards, AMD GCN and RDNA, Intel Arc - through one
> of two optical-flow backends. The NVIDIA backend uses the Optical Flow hardware
> engine of Turing-generation GPUs and newer (RTX 20/30/40/50-series and the
> GTX 1660 family, with a compatible driver; not TU117 cards such as the
> GTX 1650). On every other GPU - AMD, Intel, and older NVIDIA cards such as
> the GTX 10 series - the bridge uses the AMD FidelityFX backend automatically.
> FidelityFX computes the flow on the same GPU that renders the game, so it
> costs more frame time than the NVIDIA engine and leaves less headroom.

> [!TIP]
> A game that holds about half your headset's refresh rate can reach the full
> rate: a game at 45–50 FPS on a 90 Hz headset typically delivers 90, a
> **frame-rate increase of up to 100%**, with NVIDIA Medium at 50% optical-flow
> resolution or FidelityFX. Actual results vary by game, GPU, resolution and
> base frame rate; the bridge cannot recover frames the game does not render.
>
> A game that can only hold about a third of the refresh rate can reach it
> too, with **3X Frame Gen**: 30 FPS on a 90 Hz headset delivers 90.

The current build provides:

- AMD FidelityFX Optical Flow (the fallback: used automatically on GPUs where NVIDIA optical flow is unavailable)
- NVIDIA Optical Flow with Fast (test), Medium and Slow presets (the default, Medium)
- 100%, 75% and 50% NVIDIA optical-flow calculation scales
- a tray icon that arms the bridge as soon as it starts, with manual
  Arm/Disarm
- **Pause frame generation**: switch generation off and back on while you
  play, without restarting the game, from the tray or with a key
  (Ctrl + Alt + F7 by default, changed from the tray menu)
- **Prefer FPS over latency** (on by default): one frame of extra latency in
  exchange for reaching full frame rate from half, with smoother dips
- **3X Frame Gen** (off by default): two generated frames per game frame,
  for games at a third of your refresh rate, switchable live while you play
- **Lower VRAM** (on by default): about half a gigabyte less VRAM at high
  resolutions; untick it if that shows as stutter on your card
- a pipeline built specifically for SteamVR's compositor
- a **D3D11 bridge** (on by default): D3D11 games run on the same pipeline
  as native D3D12 games, and the OpenXR runtime never touches the game's
  D3D11 device. DCS World, Assetto Corsa, SkyrimVR and Cyberpunk 2077 run
  through it
- **Vulkan support (on by default)**: frame generation for Vulkan games,
  tested with No Man's Sky through OpenComposite. Since 0.2.10.1 Vulkan
  games run through a bridge like D3D11 games, on the same pipeline as
  native D3D12 games
- eye tracking that keeps working alongside Cheeky Foveated DLSS
- an optional transparent in-headset FPS number with four corner positions;
  green means recent synthetic submissions and red means inactive generation
- an optional bridge flight recorder for diagnostics, which can be switched
  on and off while you play

OFXR Bridge uses color-only optical flow. It does not receive game motion
vectors or depth, so artifacts around moving objects, disocclusions and head
rotation are still possible.

## Installation and use

1. Download the latest release archive from GitHub Releases.
2. Extract the complete archive to a writable folder.
3. Run `OFXRBridgeTray.exe`. The bridge arms itself straight away.
4. Right-click the tray icon to choose the optical-flow backend and options.
   Most options take effect the next time the game starts. Pause, 3X Frame
   Gen, the FPS overlay position and the flight recorder change in a running
   game.
5. Start the game normally. For injectors such as UEVR, start the tray before
   the game and leave it armed while the VR mod is injected.
6. Select **Disarm bridge** or close the tray application when finished.

### Pause and resume

To compare with and without generated frames while you play, use **Pause
frame generation** in the tray menu. The entry turns into **Resume frame
generation** with an orange pause symbol, and the FPS number in the headset
shows the same symbol (two vertical bars) until you resume. Every arm starts
resumed.

A paused game still runs through the bridge: it only stops making extra
frames. To rule OFXR out of a problem, disarm it and restart the game
instead.

**Ctrl + Alt + F7** does the same from inside the game, while the bridge is
armed. To change the key, select **Current key binding** in the tray menu,
press the key or keys you want and confirm:

- any key works, with or without Ctrl, Alt and Shift;
- **Default** goes back to Ctrl + Alt + F7 and **No key** turns the key off;
- a combination another program already uses is refused.

The game still sees the key press, so pick keys your game and mods do not
use. A key used on its own, without Ctrl, Alt or Shift, stops working in
every other program while the bridge is armed. The setting is `pause_hotkey`
under `[tray]` in `%LOCALAPPDATA%\OFXR Bridge\tray.ini`.

If arming fails at start-up, the tray shows the reason and stays disarmed;
select **Arm bridge until manual disarm** to retry.

> [!IMPORTANT]
> **Never run the game as administrator.** The bridge is registered for your
> Windows user, and the OpenXR loader deliberately ignores per-user layers in a
> program running as administrator. An elevated game runs in VR as normal but
> never loads the bridge, with no error, no FPS number and no flight log.
> Check the game's `.exe` and any shortcut or launcher you start it from
> (Properties → Compatibility → "Run this program as an administrator").
>
> The same applies to anything that *starts* the game: a launcher or mod
> manager running as administrator passes that on to the game. The UEVR
> injector is fine as administrator, since it injects into a game you already
> started normally; only a front-end that launches the game for you must not
> run elevated.

For supported NVIDIA GPUs, the suggested starting configuration is **NVIDIA
Medium** with **50% optical flow resolution**. It should provide a decent
performance boost with minimal visual-quality loss. Running the optical flow at
100% resolution is usually too expensive and often produces only a small or
negligible net performance gain, so it is not recommended for normal use.

> [!NOTE]
> **Use the OFXR FPS number, not other FPS tools, while the bridge is active.**
> Other counters can be wrong in either direction:
>
> - **fpsVR, SteamVR's frame timing and other compositor-side tools** count
>   every frame the bridge hands over. When the game runs below half the
>   refresh rate, the bridge repeats frames to fill the gaps, and these tools
>   count the repeats as new frames. They can show 90 on a 90 Hz headset while
>   far fewer new frames reach your eyes.
> - **Counters inside the game or the VR mod**, and xrFPS in some setups,
>   count the game's own frames before the bridge adds any. While frame
>   generation is active they show about half of what reaches the headset.
>   This does not mean OFXR is inactive.

The bridge's own optional FPS number counts only new frames: real and
generated, but not repeats. On SteamVR it starts from what the compositor
reports it actually showed, so frames SteamVR shows late are left out too.
On other runtimes it counts the new frames the bridge submits. See
[FPS overlay details](docs/FPS_OVERLAY.md).

When both the **Bridge flight recorder** and an FPS overlay position are
enabled, OFXR draws a small purple rectangle into synthetic frames near the FPS
counter. Its purpose is to verify whether generated frames are actually
reaching the headset: if the rectangle is visible there, the synthetic output
has reached the displayed presentation path. The green FPS number alone only
confirms accepted submissions. Disable the flight recorder after testing to
remove the marker.

The tray and bridge require the [Microsoft Visual C++ Redistributable
x64](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
OFXR Bridge does not replace your active OpenXR runtime.

FidelityFX is the most performing one but will produce artifacts during headset rotation in dark areas, this is known and cannot be avoided.

### Prefer FPS over latency

This tray option is **on by default**. The bridge holds each generated frame
back by one display refresh, so the optical flow gets a whole refresh to
finish instead of the gap the game leaves between frames.

- **Gain:** a game that only sustains about half your headset's refresh rate
  can reach the full rate. A game holding 45–50 FPS on a 90 Hz headset is
  likely to reach 90. Dips are also much smoother.
- **Cost:** one frame of extra latency, about 11 ms at 90 Hz.
- **Turn it off** when your GPU has budget to spare. Try it: turn the option
  off and play the same scene. If you still hold your full frame rate, leave
  it off and save one frame of latency. If you lose frames, turn it back on.

The change applies the next time the game starts.

### 3X Frame Gen

Tray option **3X Frame Gen [live change]**, **off by default**. The bridge
generates two frames for every frame the game renders instead of one, so a
game running at a third of your headset's refresh rate (30 FPS at 90 Hz)
reaches the full rate. If your game already holds about half the refresh
rate, the normal 2X mode looks better: leave this off.

- **Live switch:** toggle it while you play. No need to restart the game,
  the VR session or OFXR; you will see one short hitch at the switch.
- **Prefer FPS over latency** is switched on with 3X and greyed out while 3X
  is on. If the game you are playing was started with it off, 3X cannot
  switch live: a **"game restart needed"** window opens on your desktop, and
  3X applies the next time the game starts. Some games close and reopen
  their VR session (on loading, for example); the new session uses the
  settings in force at that moment.
- **Artefacts:** more than OFXR's 2X mode, fewer than SteamVR Motion Smoothing at 3X, and
  mostly in sideways motion such as strafing or flying past something close.
- **Latency**, at 90 Hz, from the game finishing a frame to that frame
  reaching the headset: 31 ms with 2X and Prefer FPS over latency, 39 ms with
  3X in a game with headroom (The Callisto Protocol), up to 49 ms in a very
  heavy one (MSFS 2024 at very high resolution). OFXR tunes this by itself
  over the first 15 seconds or so. Head rotation is corrected at display
  time in every mode.
- **Games:** D3D12, D3D11 (through the D3D11 bridge, on by default) and
  Vulkan. Tested in MSFS 2024, The Callisto Protocol, Cyberpunk 2077 and
  No Man's Sky.

### Lower VRAM

Tray option **Lower VRAM (may cause stuttering)**, **on by default**. Since
0.2.10.1 the bridge keeps one working image per output instead of two,
which saves about half a gigabyte of VRAM at high resolutions (the VRAM
table in the troubleshooting guide assumes it on). On most cards it costs
nothing. On some it shows as stutter that an older version did not have:
an AMD RX 9070 XT running AMS2 at the edge of its GPU budget was smooth on
0.2.9.1 and not on later versions, with nothing in the flight log to show
for it, and unticking this brought the smoothness back.

Untick it if you see that and have VRAM to spare. It is not a live change:
the bridge decides the layout when the game creates its VR session, so it
takes effect the next time the game starts.

### D3D12 games

Since 0.2.12.2 the bridge takes its copy of each eye's image when the game
ends its frame, not when the game hands each eye over. A game that hands the
first eye over before it has finished drawing it otherwise gave the bridge a
picture of that eye that was one frame old, which showed as flicker in one
eye only: reported in MSFS 2024 with 3X Frame Gen.

There is no menu entry. To go back to the earlier behaviour for diagnosis,
close the tray, set `capture_at_end_frame=0` under `[tray]` in
`%LOCALAPPDATA%\OFXR Bridge\tray.ini` and start the tray again.

### D3D11 games

D3D11 games go through the **D3D11 bridge**, on by default since 0.2.6. The
bridge creates the OpenXR runtime's session on its own D3D12 device; the game
keeps rendering in D3D11, into textures shared between the two devices, and
generation, pacing and submission then follow the D3D12 path that UEVR games
use. The runtime never works the game's D3D11 device, which is what crashed
NVIDIA's D3D11 driver in DCS World and SkyrimVR, and D3D11 games get the
SteamVR pacing described below. A runtime that does not offer D3D12 keeps the
previous path automatically.

There is no menu entry. To turn the bridge off for diagnosis, close the tray,
set `d3d11_bridge=0` under `[tray]` in `%LOCALAPPDATA%\OFXR Bridge\tray.ini`
and start the tray again. Editing the `ofxr_bridge.ini` beside the tray does
nothing: the tray rewrites the layer's settings from `tray.ini` each time it
arms.

### Cheeky Foveated DLSS

The bridge works alongside [Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS),
including its eye-tracked foveation, since 0.2.6. Cheeky's OpenXR layer sits
above the bridge and reads the game's frames before the bridge submits
anything, so its calibration and gaze are unaffected. If Cheeky shows a
flashing grid of small white coded squares at high resolutions, that is its
own eye calibration failing to lock, with or without the bridge; its
**Standard corners** calibration method avoids it, and setting the resolution
before launching the game helps, since every change restarts the calibration.

### Vulkan games (experimental)

Vulkan support is **on by default** and no longer has a tray menu entry.
It covers games that render through Vulkan — No Man's Sky through
[OpenComposite](https://www.nexusmods.com/nomanssky/mods/4363) is the one it
has been tested with. To turn it off, quit the tray, set `vulkan_bridge=0` in
`%LOCALAPPDATA%\OFXR Bridge\tray.ini`, and start the tray again. An older
`vulkan_support=0` line there is ignored: it was written automatically while
the option was off by default.

- While the bridge is armed, it registers a small Vulkan layer of its own
  (`OFXR_vulkan_queue_layer.dll`) that serialises GPU queue submissions,
  which the bridge's second thread otherwise races the game for. It is
  removed when you disarm or close the tray. A Vulkan implicit layer loads
  into every Vulkan application while registered, browsers included.
- **No FPS number in Vulkan games** for now: the overlay has no Vulkan path.
  The diagnostic squares still show generation running. fpsVR or the Virtual
  Desktop overlay give a rough rate, but they count repeated frames as new
  ones, so they read high whenever the game is below half the refresh rate.
- On SteamVR, turn off the game's **fixed frame rate at half** and Motion
  Smoothing in the per-application video settings, or SteamVR holds the game
  to half rate and the bridge can only deliver half.
- Since 0.2.10.1 a Vulkan game's VR session goes through a bridge, like a
  D3D11 game's: the game renders into images the bridge shares with it, and
  everything after that is the native D3D12 pipeline. That halves the VRAM
  the bridge needs and gives Vulkan games SteamVR's frame pacing. To go back
  to the earlier path, set `vulkan_session_bridge=0` in `tray.ini`.
- Off, Vulkan games are passed through unchanged and nothing Vulkan is
  registered.

### SteamVR users

SteamVR headsets (Pimax and others) get a pipeline built specifically for
SteamVR's compositor. The bridge sends two frames for every game frame, and
they must arrive one display refresh apart. Other runtimes, such as Virtual
Desktop, pace this by themselves. SteamVR does not, so the bridge times each
frame against SteamVR's compositor directly. The bridge also gives SteamVR a
GPU queue of its own, so the game's next frame can no longer make a finished
generated frame look unready; with the D3D11 bridge, D3D11 games get this
too.

For the best results on SteamVR:

- Leave **Prefer FPS over latency** on, then try the same scene with it off.
  If you still hold full frame rate without it, keep it off for one frame
  less latency.
- Pick a refresh rate close to double your game's frame rate, or triple it
  with **3X Frame Gen**. If that is still short of the refresh rate, SteamVR
  fills the gap with repeated frames and the image judders.
- If you get dips, disarm the bridge and play the same scene. If the dips
  remain, lower SteamVR's per-eye resolution; the bridge cannot recover
  frames the game does not render. Pausing is not enough for this test: a
  paused game still runs through the bridge.

### Virtual Desktop and Pimax OpenXR users

On these two runtimes the bridge chooses how it sends frames once, from how
the game itself is built, and keeps that choice for the whole session. A
given game behaves the same way every time; earlier versions could switch in
the middle of a session when the game stalled, and stay that way.

### When the number stops short of the refresh rate

Generating frames costs GPU time too, so a game needs some headroom above
half the refresh rate, on every runtime. Each pair of frames is one frame
from the game plus the optical flow and the generated frame, and the pair
has to fit in two display refreshes.

An example from our test machine, No Man's Sky on Virtual Desktop, which
runs at 94 FPS without the bridge:

| Refresh rate | Time allowed per pair | Result |
|---|---|---|
| 100 Hz | 20.0 ms | an even 100 |
| 120 Hz | 16.7 ms | 113 to 115 |
| 144 Hz | 13.9 ms | about 110 |

A pair took about 17.7 ms there: about 10.5 ms for the game's frame, about
5 ms of optical flow and generation, and the rest in handing the extra frame
over. That fits at 100 Hz and not above.

If your number stops short like this, lower the refresh rate, the per-eye
resolution or the optical-flow resolution. The refresh rate where the number
reaches the refresh rate is the one to play at.

### How much VRAM the bridge uses

See the VRAM table in [Troubleshooting](docs/TROUBLESHOOTING.md#how-much-vram-ofxr-uses).
Since 0.2.10.1 the mode no longer changes it: 2X, Prefer FPS over latency
and 3X Frame Gen all use the same amount. The table assumes **Lower VRAM**
on, which it is by default; with it off, add about 15% on D3D12 and D3D11.
The bridge also stays out of Pimax Home, which takes the headset whenever a
game leaves VR.

### What the tray changes on your PC

When the tray arms the bridge (at start-up, or when you select **Arm**), it copies the versioned OFXR layer and its
configuration into `%LOCALAPPDATA%\OFXR Bridge`, creates an absolute-path
OpenXR implicit-layer manifest and registers that manifest for the current
Windows user. It does not inject a DLL into the game, replace game files or
replace the active OpenXR runtime.

Selecting **Disarm** removes the exact OpenXR registration. Closing the tray
also disarms it, with a watchdog providing cleanup if the tray exits
unexpectedly. After disarming and closing the application, no active OFXR hook
or OpenXR registration remains on the system. Versioned cache files, settings
and diagnostic logs may remain under `%LOCALAPPDATA%\OFXR Bridge`, but they are
inert and may be deleted manually at any time.

## Reporting problems

Please report both working and non-working games, rendering problems, freezes
and crashes in [GitHub Issues](https://github.com/djules75/OFXR-Bridge/issues)
or on the [Flat2VR Modding Discord](https://discord.gg/flat2vr).

Reported results are collected in the
[OFXR Bridge Compatibility Chart](https://docs.google.com/spreadsheets/d/1lhaJm1wzt29exmx4tZbxdwf82RcrlLcyZJ850GcTf1w/edit?usp=sharing).

Before reproducing a problem:

1. Right-click the tray icon and enable **Bridge flight recorder**.
2. Start the game and reproduce the problem once. The recorder can also be
   switched on while the game is already running, right when the problem
   shows up: recording starts within a moment, and the log still begins with
   what was noted when the game started (your headset's runtime, the
   resolution, the game's graphics API). The game hitches once when you
   switch it on or off.
3. Close the game, then select **Open bridge logs** from the tray.
4. Attach the newest `ofxr-bridge-flight-*.log` file to the issue.
5. Make sure OFXR has worked on your system on at least another game before claiming that is not working for the game you are reporting

Please also include:

- game name and version
- VR mod or injector, if any
- headset and OpenXR runtime
- GPU and driver version
- Windows version/build and OFXR build number
- selected OFXR backend and options
- exact steps and the observed result

If no OFXR log was created, report that too: it usually means the layer was not
loaded or the process stopped before the recorder could start. Game logs and a
crash dump are also useful when available.

The recorder is independent from game and mod logging. It records OpenXR
negotiation, resource eligibility, frame-generation stages, recovery events
and potentially blocked call boundaries. It does not record video or replace a
native crash dump.

## Building from source

See the [Windows build instructions](docs/BUILDING.md).

## License

OFXR Bridge is licensed under [LGPL-3.0-or-later](LICENSE). Third-party
components retain their respective licenses.

## Support

If you find OFXR Bridge useful and want to support its development:

- Created by tig3rmast3r, the original author of OFXR:
  [ko-fi.com/tig3rmast3r](https://ko-fi.com/tig3rmast3r)
- 0.2.X version maintained by Djules:
  [ko-fi.com/djules](https://ko-fi.com/djules)
