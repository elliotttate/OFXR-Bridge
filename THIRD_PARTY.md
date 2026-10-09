# Third-party components

## Khronos OpenXR SDK headers

The project vendors only the four generated headers required by its Windows
layer build under `external/OpenXR-SDK/include/openxr`: `openxr.h`,
`openxr_loader_negotiation.h`, `openxr_platform.h`, and
`openxr_platform_defines.h`. They are copied without modification from Khronos
OpenXR-SDK revision `5267613edf3d937e3d77556a106a65c2f82b25c6`
(`release-1.1.61`) from
<https://github.com/KhronosGroup/OpenXR-SDK>. The files are licensed under
Apache-2.0 OR MIT; both license texts are preserved in
`external/OpenXR-SDK/LICENSES`.

| File | SHA-256 |
| --- | --- |
| `openxr.h` | `75AE85F7B1308D33572908342D1F3BF6D26A57769CC2092AA3163DFEAF1DCBED` |
| `openxr_loader_negotiation.h` | `6CEA662D58721AD77712BCDBC5620426D8C6E509ABF92AF75C9ED28B8DF9DD98` |
| `openxr_platform.h` | `150939F39AAF1159E9F59AB53B5D5863C08D5777EFA13820B5CBCD3562EB068D` |
| `openxr_platform_defines.h` | `7DC30AD27F1082A71F84F5F5ADF10762558FC9FE57FDC7628D796D22BE3B0DBE` |

This local subset makes the project build independently of any Witcher 3 VR
checkout.

## Khronos Vulkan headers

The Vulkan interop needs the Vulkan API declarations and nothing else: every
entry point is resolved at run time through the `vulkan-1.dll` the
application already loaded, so the layer links no Vulkan library and runs
unchanged where none is installed. `vk_layer.h` is the loader's layer
interface, which `OFXR_vulkan_queue_layer.dll` implements. The headers are copied without modification
from Khronos Vulkan-Headers tag `v1.3.296` from
<https://github.com/KhronosGroup/Vulkan-Headers> into
`external/Vulkan-Headers/include`. They are licensed under Apache-2.0 OR MIT,
the same terms as the OpenXR headers above; the repository's `LICENSE.md` is
preserved beside them and both license texts are in
`external/OpenXR-SDK/LICENSES`.

| File | SHA-256 |
| --- | --- |
| `vulkan/vulkan.h` | `52297513CC6B6CC8DB2921AA849F72E3F55D678B3A58A2C3EB297C02DED7A837` |
| `vulkan/vulkan_core.h` | `50AF5A157C8AAB7D90DCD929A05758B4DC3E78A619F46552BFD5CDE67B2D46C1` |
| `vulkan/vk_platform.h` | `C4CABBCF699C90CDE344095AE54DA9435D117951AAD114BA3A540C5997D7906F` |
| `vulkan/vulkan_win32.h` | `FA3A9263D1764B82B634180EC5D3010ADC216108BAE11C02B157F733073F87F8` |
| `vulkan/vk_layer.h` | `861CD0EA24C7D81B935E5726520D184DBBC00E571F9089E17A723CCBBC1944E2` |
| `vk_video/vulkan_video_codec_av1std.h` | `F3AB53DBFDEB36A349F674656D50BD507F2E4B3C883343DA74AACE0DF4F02A02` |
| `vk_video/vulkan_video_codec_av1std_decode.h` | `96F923DC01CD3E266C081D88FA020525A20D63FFE4CE09A4C807A04EF3AAF97E` |
| `vk_video/vulkan_video_codec_h264std.h` | `072EE6EAC80E73D93BB4BFE0059F99E581FC8B55B1437236185BB4FF377914B1` |
| `vk_video/vulkan_video_codec_h264std_decode.h` | `E1708B3996A740EEEDB79F51D0F0E379F9BCAE13566538375ACAF73301023B58` |
| `vk_video/vulkan_video_codec_h264std_encode.h` | `052A6E357BE8D88BF4BE0DB1B3DA5A9CC1A6011CF4B3533759FF588E1E26F9F1` |
| `vk_video/vulkan_video_codec_h265std.h` | `0CBDFD6C45EE8CA902E9C961CD7E49A4D601D443EA0EF2A60A8496B127ED397C` |
| `vk_video/vulkan_video_codec_h265std_decode.h` | `800946AA19D83D305D504E2E4141BC938F989E0CF8872CEF97480238EEFEF1EA` |
| `vk_video/vulkan_video_codec_h265std_encode.h` | `7239F9F4E811546639111E562A8BE1EC73AD5BEF1C23D5FAD80889F35E125E9C` |
| `vk_video/vulkan_video_codecs_common.h` | `78CF3C99D3D57AFA7AB1782EC6018B4BCDBE273F2843D76E3375282DB875B71E` |

## AMD FidelityFX SDK Optical Flow

The build uses the official FidelityFX SDK `v1.1.4` source at commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. Its checkout is intentionally not
committed; `docs/BUILDING.md` explains how to place it under
`external/FidelityFX-SDK-v1.1.4`. The bridge compiles and statically links only
the DX12 backend and Optical Flow 1.1.2 components:
`ffx_backend_dx12_x64.lib` and `ffx_opticalflow_x64.lib`. The checkpoint DLL has
no FidelityFX runtime-DLL dependency.

The SDK source and linked components carry AMD's MIT license; its notice is
preserved in `licenses/AMD-FidelityFX-MIT.txt`. SDK tools are build-time inputs
and are not linked into the bridge DLL.

## Valve OpenVR header

The layer reads what SteamVR's compositor actually scanned out - no OpenXR call
reports it - through `IVRCompositor::GetCumulativeStats`, the same source fpsVR
uses. That needs one public header, `openvr.h`, taken unmodified from
ValveSoftware/openvr tag `v2.5.1`:

| File | SHA-256 |
| --- | --- |
| `openvr.h` | `94E5545370159C85F87CD6E15DD3739F7C919FC7A6E869F5E4ED463533A07ED0` |

Its checkout is intentionally not committed, the same arrangement the FidelityFX
SDK has; `docs/BUILDING.md` explains how to place it under `external/openvr`.

**Nothing from OpenVR is linked or redistributed.** The bridge loads SteamVR's
own `openvr_api.dll` at runtime, located through
`%LOCALAPPDATA%\openvr\openvrpaths.vrpath`, and resolves its entry points with
`GetProcAddress` - so the layer DLL carries no import on it and runs unchanged
where SteamVR is absent. The header is a compile-time input only, and the
feature is inert on every non-SteamVR runtime.

OpenVR is licensed under BSD-3-Clause. Because the released layer binary is
built from that header, Valve's notice is reproduced in
`licenses/OpenVR-BSD-3-Clause.txt` and ships with the release archive.

## NVIDIA Optical Flow SDK interface

`XRFG-V012` uses the two public D3D12 interface headers from NVIDIA Optical
Flow SDK 5.0.7: `nvOpticalFlowCommon.h` and `nvOpticalFlowD3D12.h`. Each header
contains NVIDIA's MIT-style permission notice. The bridge does not link or
redistribute an NVIDIA SDK binary: it loads the display driver's
`nvofapi64.dll` from Windows System32 at runtime and obtains the Optical Flow
API entry points dynamically.

The NVIDIA backend uses one Optical Flow context and serializes the isolated
per-eye jobs through it. Two concurrent OFA contexts previously caused runtime
freezes, while treating both eyes as one repeated atlas produced cross-image
matches. The FidelityFX backend retains its single packed-stereo dispatch. The
full SDK package, programming guide, samples and license agreement are
development inputs and are not bridge release artifacts.

## NVIDIA DLSS Frame Generation (optional)

The `XRFG_NATIVE_DLSSG` build uses NVIDIA/DLSS SDK 310.9.1 at commit
`374959484e79a640feaba44c93ac8cfb0a03f5b5`. The SDK checkout is a build input
outside the repository. This option statically links the NGX D3D12 interface
and distributes the unmodified production `nvngx_dlssg.dll` feature runtime
beside the layer. It does not distribute the SDK source, tools or development
runtime. The SDK's license is preserved in `licenses/NVIDIA-DLSS.txt` and
included with native-enabled builds. See `docs/BUILDING.md` for setup and the
guide-input requirements.

Native-enabled layers independently capture the application's public NGX
upscaler inputs. The interceptor statically links SafetyHook v0.6.9 at
`c3f3f306a0f12d1811c0b713ad2ed2a8ddc6cf55` (Boost Software License 1.0),
Zydis 4.1.0 and Zycore (MIT). Notices are preserved in
`licenses/SafetyHook-Boost-1.0.txt`, `licenses/Zydis-MIT.txt` and
`licenses/Zycore-MIT.txt`.

`include/xrfg/third_party/uevr_api.h` is UEVR's public SDK 2.41 header from
`11ad244a9911ee213b79c254a7e1254fb0e84a5f`. That header is separately licensed
under MIT; its notice is preserved in the header and `licenses/UEVR-API-MIT.txt`.
The bridge uses the exported SDK to read renderer projection metadata. No UEVR
implementation source or OptiScaler source is incorporated into this capture.

## Candidate: Khronos OpenXR-SDK-Source API layer scaffold

Not copied. `XRFG-V001` through `XRFG-V003` follow the public loader/API-layer
negotiation contract and were cross-checked against the Khronos API dump layer.
If source is imported later, retain its Apache-2.0/MIT notices and exact
revision.
