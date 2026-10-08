#include "xrfg/d3d12_native_dlssg.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <share.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

#ifdef XRFG_NATIVE_DLSSG
#include "native_dlssg_pack_shader.hpp"
#include "native_dlssg_pixel_shader.hpp"
#include "native_dlssg_vertex_shader.hpp"
#include <DirectXMath.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#endif

namespace xrfg {
#ifdef XRFG_NATIVE_DLSSG
namespace {
using Microsoft::WRL::ComPtr;
using namespace DirectX;
constexpr UINT kSlots = D3D12SwapchainHistory::kSlotCount;
constexpr UINT kBlock = 9;        // six SRVs, three UAVs
constexpr UINT kBlocksPerEye = 4; // A, B, and two outputs
constexpr UINT kMaxOutputs = 2;
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kPixelRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto kAnyRead = kRead | kPixelRead;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto kCommon = D3D12_RESOURCE_STATE_COMMON;
// DLSS-G interpolates display-ready colour, so the pack shader encodes sRGB
// views. Ten bits per channel cost no more memory than the 8-bit swapchain
// and keep the resampled previous frame from rounding to 8-bit codes; the
// generated alpha is not used, so its two bits do not matter.
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr DXGI_FORMAT kFallbackColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// A camera change that moves no pixel of the view further than this keeps the
// NGX history instead of reseeding it with the aligned previous frame.
constexpr float kReseedPixels = 0.1F;
// Pairs skipped before another attempt after NGX could not create a feature.
constexpr UINT kCreateRetryPairs = 120;

struct Params {
    UINT extent[2], color_slice, guide_slice;
    float output_rect[4], motion_rect[4], depth_rect[4];
    float motion_scale[2], jitter_delta[2], rotation[4];
    float source_tangents[4], target_tangents[4];
    UINT motion_slice, reversed_depth, pack_previous, encode_srgb;
    float depth_convention[4];
};
static_assert(sizeof(Params) == 40 * sizeof(UINT));

// Batches transitions and remembers each resource's resting state, so a
// record that stops early returns everything it touched to that state.
class Transitions {
  public:
    explicit Transitions(ID3D12GraphicsCommandList *list) noexcept : list_(list) {}
    void to(ID3D12Resource *r, D3D12_RESOURCE_STATES rest, D3D12_RESOURCE_STATES state) {
        if (!r) {
            return;
        }
        Entry *entry = find(r);
        if (!entry) {
            // Two eyes touch at most 26 distinct resources.
            if (tracked_count_ == tracked_.size()) {
                throw std::length_error("native DLSS-G transitions");
            }
            entry = &tracked_[tracked_count_++];
            *entry = {r, rest, rest};
        }
        if (entry->state == state) {
            return;
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, entry->state, state};
        push(b);
        entry->state = state;
    }
    // Orders two NGX evaluations that write the same output.
    void uav(ID3D12Resource *r) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = r;
        push(b);
    }
    void flush() noexcept {
        if (pending_count_) {
            list_->ResourceBarrier(pending_count_, pending_.data());
            pending_count_ = 0;
        }
    }
    void restore() {
        for (UINT i = 0; i < tracked_count_; ++i) {
            to(tracked_[i].resource, tracked_[i].rest, tracked_[i].rest);
        }
        flush();
    }

  private:
    struct Entry {
        ID3D12Resource *resource;
        D3D12_RESOURCE_STATES rest, state;
    };
    Entry *find(ID3D12Resource *r) noexcept {
        for (UINT i = 0; i < tracked_count_; ++i) {
            if (tracked_[i].resource == r) {
                return &tracked_[i];
            }
        }
        return nullptr;
    }
    void push(const D3D12_RESOURCE_BARRIER &b) noexcept {
        if (pending_count_ == pending_.size()) {
            flush();
        }
        pending_[pending_count_++] = b;
    }
    ID3D12GraphicsCommandList *list_;
    std::array<Entry, 32> tracked_{};
    std::array<D3D12_RESOURCE_BARRIER, 32> pending_{};
    UINT tracked_count_{}, pending_count_{};
};

bool srgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
DXGI_FORMAT motion_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16_TYPELESS   ? DXGI_FORMAT_R16G16_FLOAT
           : f == DXGI_FORMAT_R32G32_TYPELESS ? DXGI_FORMAT_R32G32_FLOAT
                                              : f;
}
DXGI_FORMAT depth_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return f;
    }
}
bool same_device(ID3D12Device *d, ID3D12DeviceChild *r) {
    if (!r) {
        return false;
    }
    ComPtr<ID3D12Device> rd;
    ComPtr<IUnknown> x, y;
    return SUCCEEDED(r->GetDevice(IID_PPV_ARGS(&rd))) &&
           SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&x))) &&
           SUCCEEDED(rd->QueryInterface(IID_PPV_ARGS(&y))) && x.Get() == y.Get();
}
bool valid_rect(ID3D12Resource *r, UINT x, UINT y, UINT w, UINT h, UINT slice) {
    if (!r || !w || !h) {
        return false;
    }
    const auto d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1 &&
           slice < d.DepthOrArraySize && !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
           std::uint64_t(x) + w <= d.Width && std::uint64_t(y) + h <= d.Height;
}
D3D12ImageRect rect(const D3D12ReprojectionView &v, UINT w, UINT h) {
    return v.image_rect.width && v.image_rect.height ? v.image_rect : D3D12ImageRect{0, 0, w, h};
}
UINT slice(const D3D12ReprojectionView &v, UINT index, UINT views, UINT arrays) {
    return v.array_slice != std::numeric_limits<UINT>::max() ? v.array_slice
           : arrays == views                                 ? index
                                                             : 0;
}
XMVECTOR orientation(const D3D12ReprojectionView &v) {
    const auto q = normalize(v.pose.orientation);
    return XMVectorSet(q.x, q.y, q.z, q.w);
}
// Rotates a B-camera ray into A's camera, as the pack shader does.
XMVECTOR b_to_a(const D3D12ReprojectionView &a, const D3D12ReprojectionView &b) {
    return XMQuaternionMultiply(orientation(b), XMQuaternionConjugate(orientation(a)));
}
std::array<float, 4> tangents(const D3D12FieldOfView &f) {
    return {std::tan(f.angle_left), std::tan(f.angle_right), std::tan(f.angle_up),
            std::tan(f.angle_down)};
}
// The furthest any pixel of B's view moves when A is aligned into B's camera.
// The alignment is rotational, so a translation alone never moves a pixel.
float alignment_pixels(const D3D12ReprojectionView &a, const D3D12ReprojectionView &b,
                       UINT width, UINT height) {
    const auto q = b_to_a(a, b);
    const auto at = tangents(a.fov), bt = tangents(b.fov);
    float furthest = 0;
    for (const float u : {0.0F, 0.5F, 1.0F}) {
        for (const float v : {0.0F, 0.5F, 1.0F}) {
            const auto ray = XMVector3Rotate(
                XMVectorSet(bt[0] + (bt[1] - bt[0]) * u, bt[2] + (bt[3] - bt[2]) * v, -1, 0), q);
            const float z = -XMVectorGetZ(ray);
            if (z <= 0.00001F) {
                return std::numeric_limits<float>::infinity();
            }
            const float au = (XMVectorGetX(ray) / z - at[0]) / (at[1] - at[0]);
            const float av = (XMVectorGetY(ray) / z - at[2]) / (at[3] - at[2]);
            furthest = std::max({furthest, std::abs(au - u) * width, std::abs(av - v) * height});
        }
    }
    return furthest;
}
XMMATRIX projection(const D3D12FieldOfView &f, const DlssMotionVectorFrame &g) {
    const float l = std::tan(f.angle_left), r = std::tan(f.angle_right);
    const float t = std::tan(f.angle_up), b = std::tan(f.angle_down);
    const float n = g.camera_near, far_plane = g.camera_far;
    const float z = g.depth_infinite   ? (g.depth_inverted ? 0.f : -1.f)
                    : g.depth_inverted ? n / (far_plane - n)
                                       : far_plane / (n - far_plane);
    const float zw = g.depth_infinite   ? (g.depth_inverted ? n : -n)
                     : g.depth_inverted ? n * far_plane / (far_plane - n)
                                        : n * far_plane / (n - far_plane);
    return XMMATRIX(2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, (r + l) / (r - l),
                    (t + b) / (t - b), z, -1, 0, 0, zw, 0);
}
XMMATRIX camera(const Pose &p) {
    const auto q = normalize(p.orientation);
    return XMMatrixRotationQuaternion(XMVectorSet(q.x, q.y, q.z, q.w)) *
           XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
}
void matrix(float (&out)[4][4], FXMMATRIX m) {
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4 *>(out), m);
}
std::filesystem::path module_directory() {
    HMODULE m{};
    wchar_t p[32768]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_directory), &m);
    GetModuleFileNameW(m, p, 32768);
    return std::filesystem::path(p).parent_path();
}
bool environment(const wchar_t *name) {
    return GetEnvironmentVariableW(name, nullptr, 0) != 0;
}
// Initializes NGX for the device while an OFXR feature uses it. NGX is never
// shut down here: the driver keeps one NGX instance per adapter for the whole
// process, and NVSDK_NGX_D3D12_Shutdown1 tears it down for every client - it
// shuts down each loaded feature module for the device, including the
// game's own DLSS upscaler that this path captures guides from.
struct DeviceUse {
    ComPtr<ID3D12Device> device;
    UINT users;
};
struct AdapterFrames {
    LUID adapter;
    std::uint32_t frames;
};
std::mutex ngx_mutex;
std::vector<DeviceUse> ngx_devices;
std::vector<AdapterFrames> ngx_frames;
HRESULT acquire_ngx(ID3D12Device *d) {
    std::scoped_lock lock(ngx_mutex);
    for (auto &e : ngx_devices) {
        if (e.device.Get() == d) {
            ++e.users;
            return S_OK;
        }
    }
    const auto dir = module_directory();
    const wchar_t *paths[]{dir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = {paths, 1};
    if (environment(L"XRFG_TEST_NATIVE_DLSSG_VERBOSE")) {
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
        info.LoggingInfo.LoggingCallback = [](const char *message, NVSDK_NGX_Logging_Level,
                                              NVSDK_NGX_Feature) {
            std::fprintf(stderr, "NGX: %s\n", message);
        };
    }
    wchar_t local[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    auto logs = *local ? std::filesystem::path(local) / L"OFXR Bridge" / L"NGX" : dir / L"NGX";
    std::filesystem::create_directories(logs);
    const auto result = NVSDK_NGX_D3D12_Init_with_ProjectID("2152e8d8-a3d7-4d4c-88a5-37f740f81036",
                                                            NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                            "OFXR-Bridge", logs.c_str(), d, &info);
    if (NVSDK_NGX_FAILED(result)) {
        return DXGI_ERROR_UNSUPPORTED;
    }
    ngx_devices.push_back({d, 1});
    return S_OK;
}
void release_ngx(ID3D12Device *d) {
    std::scoped_lock lock(ngx_mutex);
    for (auto it = ngx_devices.begin(); it != ngx_devices.end(); ++it) {
        if (it->device.Get() == d) {
            if (--it->users == 0) {
                ngx_devices.erase(it);
            }
            return;
        }
    }
}
// Requires acquire_ngx. Zero when frame generation is unavailable.
std::uint32_t query_generated_frames() {
    NVSDK_NGX_Parameter *caps{};
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) || !caps) {
        return 0;
    }
    int available = 0;
    unsigned int frames = 1;
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &frames);
    NVSDK_NGX_D3D12_DestroyParameters(caps);
    return available ? std::max(frames, 1U) : 0;
}
bool same_adapter(const LUID &x, const LUID &y) {
    return x.LowPart == y.LowPart && x.HighPart == y.HighPart;
}
void remember_frames(ID3D12Device *d, std::uint32_t frames) {
    const LUID adapter = d->GetAdapterLuid();
    std::scoped_lock lock(ngx_mutex);
    for (const auto &e : ngx_frames) {
        if (same_adapter(e.adapter, adapter)) {
            return;
        }
    }
    ngx_frames.push_back({adapter, frames});
}
} // namespace

struct D3D12NativeDlssG::Impl {
    struct DecisionReadback {
        ComPtr<ID3D12Resource> buffer;
        ComPtr<ID3D12Fence> fence;
        std::uint64_t value{}, publication{};
        UINT eyes{}, outputs{};
    };
    struct Eye {
        NVSDK_NGX_Handle *handle{};
        NVSDK_NGX_Parameter *params{};
        ComPtr<ID3D12Resource> color, motion, depth;
        std::array<ComPtr<ID3D12Resource>, kMaxOutputs> generated, disable;
        std::uint64_t stream{}, epoch{}, serial{};
        UINT width{}, height{}, outputs{};
    };
    // What each descriptor block last described. Holding the resources means
    // an unchanged pointer is the same resource, never a reused address.
    struct Block {
        std::array<ComPtr<ID3D12Resource>, kBlock> resources;
        std::array<DXGI_FORMAT, 3> formats{};
    };
    struct Target {
        ComPtr<ID3D12Resource> image;
        UINT slice{};
    };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pack, compose;
    std::array<ComPtr<ID3D12DescriptorHeap>, kSlots> heaps;
    std::array<ComPtr<ID3D12DescriptorHeap>, kSlots> rtvs;
    std::array<std::array<Block, 2 * kBlocksPerEye>, kSlots> blocks;
    std::array<std::array<Target, 2 * kMaxOutputs>, kSlots> targets;
    std::array<Eye, 2> eyes;
    D3D12_RESOURCE_DESC source{};
    DXGI_FORMAT view_format{}, color_format{kColorFormat};
    UINT increment{}, rtv_increment{}, max_outputs{1};
    bool acquired{}, encode_srgb{};
    ComPtr<ID3D12Fence> completion;
    std::uint64_t completion_value{};
    std::array<DecisionReadback, kSlots> decisions;
    UINT recorded_slot{kSlots};
    bool diagnostic_decisions{}, capture_only{}, test_output{};
    FILE *decision_log{};
    Skip skip{Skip::none};
    UINT create_retry{};
    std::uint64_t reseeds{}, failures{};
    void report(const char *what, unsigned code) noexcept {
        // The first failures and then one in 300: a persistent NGX rejection
        // must not flood the debugger once per pair.
        if (++failures > 8 && failures % 300 != 0) {
            return;
        }
        char message[160]{};
        std::snprintf(message, sizeof(message), "OFXR native DLSS FG %s: 0x%x (failure %llu)\n",
                      what, code, static_cast<unsigned long long>(failures));
        OutputDebugStringA(message);
        if (test_output) {
            std::fputs(message, stderr);
        }
    }
    void poll_decisions() noexcept {
        try {
        if (diagnostic_decisions && !decision_log) {
            const auto path = module_directory() /
                (L"ofxr-native-decisions-pid" + std::to_wstring(GetCurrentProcessId()) +
                 L"-instance" + std::to_wstring(reinterpret_cast<std::uintptr_t>(this)) + L".log");
            decision_log = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
        }
        for (auto &readback : decisions) {
            if (!readback.fence || !readback.value ||
                readback.fence->GetCompletedValue() < readback.value) continue;
            const D3D12_RANGE range{0, 4 * sizeof(UINT)};
            void *mapped{};
            if (SUCCEEDED(readback.buffer->Map(0, &range, &mapped))) {
                const auto *flags = static_cast<const UINT *>(mapped);
                if (decision_log) {
                    std::fprintf(decision_log,
                        "%llu publication=%llu eyes=%u outputs=%u disable=%u,%u,%u,%u\n",
                        GetTickCount64(), readback.publication, readback.eyes, readback.outputs,
                        flags[0], readback.outputs > 1 ? flags[1] : 0,
                        readback.eyes > 1 ? flags[2] : 0,
                        readback.eyes > 1 && readback.outputs > 1 ? flags[3] : 0);
                    std::fflush(decision_log);
                }
                const D3D12_RANGE no_write{0, 0};
                readback.buffer->Unmap(0, &no_write);
            }
            readback.value = 0;
            readback.fence.Reset();
        }
        } catch (...) {}
    }
    ~Impl() {
        poll_decisions();
        if (decision_log) std::fclose(decision_log);
        for (auto &e : eyes) {
            if (e.handle) {
                NVSDK_NGX_D3D12_ReleaseFeature(e.handle);
            }
            if (e.params) {
                NVSDK_NGX_D3D12_DestroyParameters(e.params);
            }
        }
        if (acquired) {
            release_ngx(device.Get());
        }
    }
    HRESULT texture(DXGI_FORMAT format, ComPtr<ID3D12Resource> &out, bool buffer, UINT width,
                    UINT height) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = buffer ? D3D12_RESOURCE_DIMENSION_BUFFER : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = buffer ? 4 : width;
        d.Height = buffer ? 1 : height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Format = buffer ? DXGI_FORMAT_UNKNOWN : format;
        d.Layout = buffer ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = hp.VisibleNodeMask = 1;
        return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&out));
    }
    void release_eye(Eye &eye) noexcept {
        if (eye.handle) {
            NVSDK_NGX_D3D12_ReleaseFeature(eye.handle);
        }
        if (eye.params) {
            NVSDK_NGX_D3D12_DestroyParameters(eye.params);
        }
        eye = {};
    }
    HRESULT create_eye(ID3D12GraphicsCommandList *list, Eye &eye, UINT width, UINT height) {
        release_eye(eye);
        HRESULT hr = texture(color_format, eye.color, false, width, height);
        if (FAILED(hr)) {
            return hr;
        }
        hr = texture(DXGI_FORMAT_R16G16_FLOAT, eye.motion, false, width, height);
        if (FAILED(hr)) {
            return hr;
        }
        hr = texture(DXGI_FORMAT_R32_FLOAT, eye.depth, false, width, height);
        if (FAILED(hr)) {
            return hr;
        }
        eye.width = width;
        eye.height = height;
        hr = ensure_outputs(eye, 1);
        if (FAILED(hr)) {
            return hr;
        }
        auto result = NVSDK_NGX_D3D12_AllocateParameters(&eye.params);
        if (NVSDK_NGX_FAILED(result)) {
            return E_FAIL;
        }
        NVSDK_NGX_DLSSG_Create_Params create{};
        create.Width = create.RenderWidth = width;
        create.Height = create.RenderHeight = height;
        create.NativeBackbufferFormat = color_format;
        result = NGX_D3D12_CREATE_DLSSG(list, 1, 1, &eye.handle, eye.params, &create);
        if (NVSDK_NGX_FAILED(result)) {
            eye.handle = nullptr;
            return DXGI_ERROR_UNSUPPORTED;
        }
        return S_OK;
    }
    // The second generated image exists only once 3X asks for it.
    HRESULT ensure_outputs(Eye &eye, UINT count) {
        for (; eye.outputs < count; ++eye.outputs) {
            HRESULT hr = texture(color_format, eye.generated[eye.outputs], false, eye.width,
                                 eye.height);
            if (FAILED(hr)) {
                return hr;
            }
            hr = texture(DXGI_FORMAT_UNKNOWN, eye.disable[eye.outputs], true, 0, 0);
            if (FAILED(hr)) {
                return hr;
            }
        }
        return S_OK;
    }
    HRESULT pipelines() {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0};
        D3D12_ROOT_PARAMETER rp[3]{};
        for (UINT i = 0; i < 2; ++i) {
            rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[i].DescriptorTable = {1, &ranges[i]};
        }
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[2].Constants = {0, 0, 40};
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 3;
        rd.pParameters = rp;
        ComPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr)) {
            return hr;
        }
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
        if (FAILED(hr)) {
            return hr;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cs{};
        cs.pRootSignature = root.Get();
        cs.CS = {g_xrfg_native_dlssg_pack_shader, sizeof(g_xrfg_native_dlssg_pack_shader)};
        hr = device->CreateComputePipelineState(&cs, IID_PPV_ARGS(&pack));
        if (FAILED(hr)) {
            return hr;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = root.Get();
        ps.VS = {g_xrfg_native_dlssg_vertex_shader, sizeof(g_xrfg_native_dlssg_vertex_shader)};
        ps.PS = {g_xrfg_native_dlssg_pixel_shader, sizeof(g_xrfg_native_dlssg_pixel_shader)};
        ps.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        ps.SampleMask = UINT_MAX;
        ps.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        ps.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ps.RasterizerState.DepthClipEnable = TRUE;
        ps.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        ps.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        ps.NumRenderTargets = 1;
        ps.RTVFormats[0] = view_format;
        ps.SampleDesc.Count = 1;
        return device->CreateGraphicsPipelineState(&ps, IID_PPV_ARGS(&compose));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT slot, UINT base) {
        auto h = heaps[slot]->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(base) * increment;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT slot, UINT base) {
        auto h = heaps[slot]->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(base) * increment;
        return h;
    }
    void srv(UINT slot, UINT base, ID3D12Resource *r, DXGI_FORMAT format, bool array) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = format;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.ViewDimension =
            array ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (array) {
            d.Texture2DArray.MipLevels = 1;
            d.Texture2DArray.ArraySize = r->GetDesc().DepthOrArraySize;
        } else {
            d.Texture2D.MipLevels = 1;
        }
        device->CreateShaderResourceView(r, &d, cpu(slot, base));
    }
    // A work slot is reused only after its previous submission completed, so
    // a block can be rewritten here; one that still matches is left alone.
    void descriptors(UINT slot, UINT base, Eye &e, ID3D12Resource *color,
                     const DlssMotionVectorFrame &g, UINT output, ID3D12Resource *fallback) {
        const std::array<ID3D12Resource *, kBlock> resources{
            color, g.motion_vectors.Get(), g.depth.Get(), e.generated[output].Get(),
            e.disable[output].Get(), fallback, e.color.Get(), e.motion.Get(), e.depth.Get()};
        const std::array<DXGI_FORMAT, 3> formats{view_format,
                                                 motion_format(g.motion_vectors->GetDesc().Format),
                                                 depth_format(g.depth->GetDesc().Format)};
        auto &written = blocks[slot][base / kBlock];
        bool current = written.formats == formats;
        for (UINT i = 0; current && i < kBlock; ++i) {
            current = written.resources[i].Get() == resources[i];
        }
        if (current) {
            return;
        }
        srv(slot, base, color, formats[0], true);
        srv(slot, base + 1, resources[1], formats[1], true);
        srv(slot, base + 2, resources[2], formats[2], true);
        srv(slot, base + 3, resources[3], color_format, false);
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R32_TYPELESS;
        raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raw.Buffer.NumElements = 1;
        raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(resources[4], &raw, cpu(slot, base + 4));
        srv(slot, base + 5, fallback, formats[0], true);
        const DXGI_FORMAT ff[]{color_format, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT};
        for (UINT i = 0; i < 3; ++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = ff[i];
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(resources[6 + i], nullptr, &u,
                                              cpu(slot, base + 6 + i));
        }
        for (UINT i = 0; i < kBlock; ++i) {
            written.resources[i] = resources[i];
        }
        written.formats = formats;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE target(UINT slot, UINT index, ID3D12Resource *image,
                                       UINT array_slice) {
        auto h = rtvs[slot]->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(index) * rtv_increment;
        auto &written = targets[slot][index];
        if (written.image.Get() != image || written.slice != array_slice) {
            D3D12_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = view_format;
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.FirstArraySlice = array_slice;
            rd.Texture2DArray.ArraySize = 1;
            device->CreateRenderTargetView(image, &rd, h);
            written = {image, array_slice};
        }
        return h;
    }
    Params parameters(const D3D12ReprojectionView &view, const DlssMotionVectorFrame &g,
                      UINT view_index, UINT views) {
        Params p{};
        const auto r = rect(view, UINT(source.Width), source.Height);
        p.extent[0] = r.width;
        p.extent[1] = r.height;
        p.color_slice = slice(view, view_index, views, source.DepthOrArraySize);
        p.guide_slice = g.depth->GetDesc().DepthOrArraySize == 1 ? 0 : g.output_slice;
        p.motion_slice = g.motion_slice;
        p.reversed_depth = g.depth_inverted;
        p.encode_srgb = encode_srgb;
        p.depth_convention[0] = g.camera_near;
        p.depth_convention[1] = g.camera_far;
        p.depth_convention[2] = g.depth_infinite ? 1.0F : 0.0F;
        p.output_rect[0] = float(r.offset_x);
        p.output_rect[1] = float(r.offset_y);
        p.output_rect[2] = float(r.width);
        p.output_rect[3] = float(r.height);
        const float ux = float(r.offset_x - g.output_x) / g.output_width,
                    uy = float(r.offset_y - g.output_y) / g.output_height;
        const float uw = float(r.width) / g.output_width, uh = float(r.height) / g.output_height;
        p.motion_rect[0] = g.motion_x + ux * g.motion_width;
        p.motion_rect[1] = g.motion_y + uy * g.motion_height;
        p.motion_rect[2] = uw * g.motion_width;
        p.motion_rect[3] = uh * g.motion_height;
        p.depth_rect[0] = g.depth_x + ux * g.depth_width;
        p.depth_rect[1] = g.depth_y + uy * g.depth_height;
        p.depth_rect[2] = uw * g.depth_width;
        p.depth_rect[3] = uh * g.depth_height;
        p.motion_scale[0] = g.scale_x * float(g.output_width) / g.motion_width;
        p.motion_scale[1] = g.scale_y * float(g.output_height) / g.motion_height;
        if (g.jittered) {
            p.jitter_delta[0] =
                (g.jitter_x - g.previous_jitter_x) * float(g.output_width) / g.motion_width;
            p.jitter_delta[1] =
                (g.jitter_y - g.previous_jitter_y) * float(g.output_height) / g.motion_height;
        }
        return p;
    }
    void pack_input(ID3D12GraphicsCommandList *list, Transitions &t, UINT slot, UINT base,
                    Eye &e, ID3D12Resource *color, const DlssMotionVectorFrame &g,
                    const Params &p, ID3D12Resource *fallback) {
        descriptors(slot, base, e, color, g, 0, fallback);
        t.to(g.motion_vectors.Get(), g.resource_state, kRead);
        t.to(g.depth.Get(), g.depth_resource_state, kRead);
        for (auto *r : {e.color.Get(), e.motion.Get(), e.depth.Get()}) {
            t.to(r, kCommon, kWrite);
        }
        t.flush();
        ID3D12DescriptorHeap *hh[]{heaps[slot].Get()};
        list->SetDescriptorHeaps(1, hh);
        list->SetComputeRootSignature(root.Get());
        list->SetPipelineState(pack.Get());
        list->SetComputeRootDescriptorTable(0, gpu(slot, base));
        list->SetComputeRootDescriptorTable(1, gpu(slot, base + 6));
        list->SetComputeRoot32BitConstants(2, 40, &p, 0);
        list->Dispatch((p.extent[0] + 7) / 8, (p.extent[1] + 7) / 8, 1);
        // NGX reads its inputs as non-pixel shader resources.
        for (auto *r : {e.color.Get(), e.motion.Get(), e.depth.Get()}) {
            t.to(r, kCommon, kRead);
        }
    }
    HRESULT evaluate(ID3D12GraphicsCommandList *list, Transitions &t, Eye &e,
                     const D3D12ReprojectionView &b, const DlssMotionVectorFrame &gb,
                     const Params &p, UINT count, UINT index, bool reset) {
        const UINT output = index - 1;
        t.to(e.generated[output].Get(), kCommon, kWrite);
        t.to(e.disable[output].Get(), kCommon, kWrite);
        t.flush();
        NVSDK_NGX_D3D12_DLSSG_Eval_Params in{};
        in.pBackbuffer = e.color.Get();
        in.pMVecs = e.motion.Get();
        in.pDepth = e.depth.Get();
        in.pOutputInterpFrame = e.generated[output].Get();
        in.pOutputDisableInterpolation = e.disable[output].Get();
        NVSDK_NGX_DLSSG_Opt_Eval_Params o{};
        // Both packed frames are in B's camera plane, so the camera matrices
        // describe no rotation between them; the engine vectors carry the rest.
        const auto bp = projection(b.fov, gb);
        matrix(o.cameraViewToClip, bp);
        matrix(o.clipToCameraView, XMMatrixInverse(nullptr, bp));
        matrix(o.clipToLensClip, XMMatrixIdentity());
        matrix(o.clipToPrevClip, XMMatrixIdentity());
        matrix(o.prevClipToClip, XMMatrixIdentity());
        const auto c = camera(b.pose);
        XMFLOAT3 right, up, fwd;
        XMStoreFloat3(&right, XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), c));
        XMStoreFloat3(&up, XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), c));
        XMStoreFloat3(&fwd, XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), c));
        o.cameraRight[0] = right.x;
        o.cameraRight[1] = right.y;
        o.cameraRight[2] = right.z;
        o.cameraUp[0] = up.x;
        o.cameraUp[1] = up.y;
        o.cameraUp[2] = up.z;
        o.cameraFwd[0] = fwd.x;
        o.cameraFwd[1] = fwd.y;
        o.cameraFwd[2] = fwd.z;
        o.cameraPos[0] = b.pose.position.x;
        o.cameraPos[1] = b.pose.position.y;
        o.cameraPos[2] = b.pose.position.z;
        o.cameraNear = gb.camera_near;
        o.cameraFar = gb.camera_far;
        o.cameraFOV = b.fov.angle_up - b.fov.angle_down;
        o.cameraAspectRatio = float(p.extent[0]) / p.extent[1];
        o.depthInverted = gb.depth_inverted;
        o.cameraMotionIncluded = true;
        o.motionVectorsDilated = true;
        o.motionVectorsInvalidValue = std::numeric_limits<float>::max();
        // NGX takes motion in pixels of the motion texture. The pack writes
        // it as a fraction of the view, so scale by the packed extent.
        o.mvecScale[0] = float(p.extent[0]);
        o.mvecScale[1] = float(p.extent[1]);
        o.reset = reset;
        // Colour and motion are unjittered by now, and NGX's result does not
        // depend on this offset for them. Depth still carries the render
        // jitter, resampled onto the output grid, so express it in that grid.
        o.jitterOffset[0] = gb.jitter_x * float(gb.output_width) / gb.depth_width;
        o.jitterOffset[1] = gb.jitter_y * float(gb.output_height) / gb.depth_height;
        o.multiFrameCount = count;
        o.multiFrameIndex = index;
        o.mvecsSubrectSize = o.depthSubrectSize =
            o.backbufferSubrectSize = {p.extent[0], p.extent[1]};
        const auto result = NGX_D3D12_EVALUATE_DLSSG(list, e.handle, e.params, &in, &o);
        if (NVSDK_NGX_FAILED(result)) {
            report("evaluate failed", unsigned(result));
            return E_FAIL;
        }
        return S_OK;
    }
};
#else
struct D3D12NativeDlssG::Impl {};
#endif

D3D12NativeDlssG::D3D12NativeDlssG() = default;
D3D12NativeDlssG::~D3D12NativeDlssG() = default;

HRESULT D3D12NativeDlssG::initialize(ID3D12Device *device, ID3D12CommandQueue *queue,
                                     const D3D12_RESOURCE_DESC &source,
                                     DXGI_FORMAT format) noexcept {
#ifdef XRFG_NATIVE_DLSSG
    try {
        auto p = std::make_unique<Impl>();
        p->device = device;
        p->queue = queue;
        p->source = source;
        p->view_format = format;
        p->encode_srgb = srgb(format);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{kColorFormat};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                               sizeof(support))) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
            p->color_format = kFallbackColorFormat;
        }
        p->diagnostic_decisions = environment(L"XRFG_TEST_NATIVE_DLSSG_DECISIONS");
        p->capture_only = environment(L"XRFG_TEST_NATIVE_DLSSG_CAPTURE_ONLY");
        p->test_output = environment(L"XRFG_TEST_NATIVE_DLSSG");
        HRESULT hr = acquire_ngx(device);
        if (FAILED(hr)) {
            return hr;
        }
        p->acquired = true;
        const std::uint32_t frames = query_generated_frames();
        remember_frames(device, frames);
        if (!frames) {
            return DXGI_ERROR_UNSUPPORTED;
        }
        p->max_outputs = frames;
        hr = p->pipelines();
        if (FAILED(hr)) {
            return hr;
        }
        p->increment =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        p->rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT slot = 0; slot < kSlots; ++slot) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 2 * kBlocksPerEye * kBlock;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->heaps[slot]));
            if (FAILED(hr)) {
                return hr;
            }
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = 2 * kMaxOutputs;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->rtvs[slot]));
            if (FAILED(hr)) {
                return hr;
            }
            if (p->diagnostic_decisions) {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_READBACK;
                hp.CreationNodeMask = hp.VisibleNodeMask = 1;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = 4 * sizeof(UINT);
                bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&p->decisions[slot].buffer));
                if (FAILED(hr)) return hr;
            }
        }
        impl_ = std::move(p);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
#else
    (void)device;
    (void)queue;
    (void)source;
    (void)format;
    return DXGI_ERROR_UNSUPPORTED;
#endif
}
void D3D12NativeDlssG::reset() noexcept {
#ifdef XRFG_NATIVE_DLSSG
    if (impl_) {
        for (auto &e : impl_->eyes) {
            e.serial = 0;
        }
    }
#endif
}
void D3D12NativeDlssG::submitted(ID3D12Fence *fence, std::uint64_t value) noexcept {
#ifdef XRFG_NATIVE_DLSSG
    if (impl_) {
        impl_->completion = fence;
        impl_->completion_value = value;
        if (impl_->diagnostic_decisions && impl_->recorded_slot < kSlots) {
            auto &readback = impl_->decisions[impl_->recorded_slot];
            readback.fence = fence;
            readback.value = value;
            impl_->recorded_slot = kSlots;
        }
    }
#else
    (void)fence;
    (void)value;
#endif
}
D3D12NativeDlssG::Skip D3D12NativeDlssG::last_skip() const noexcept {
#ifdef XRFG_NATIVE_DLSSG
    return impl_ ? impl_->skip : Skip::none;
#else
    return Skip::none;
#endif
}
std::uint64_t D3D12NativeDlssG::reseeds() const noexcept {
#ifdef XRFG_NATIVE_DLSSG
    return impl_ ? impl_->reseeds : 0;
#else
    return 0;
#endif
}

HRESULT D3D12NativeDlssG::record(ID3D12GraphicsCommandList *list, UINT slot, ID3D12Resource *a,
                                 ID3D12Resource *b, std::span<const D3D12ReprojectionView> av,
                                 std::span<const D3D12ReprojectionView> bv,
                                 const DlssMotionVectorSet *ag, const DlssMotionVectorSet *bg,
                                 std::span<const Output> outputs,
                                 D3D12_RESOURCE_STATES release) noexcept {
#ifdef XRFG_NATIVE_DLSSG
    try {
        if (!impl_ || slot >= kSlots || !list || !a || !b || av.empty() || av.size() != bv.size() ||
            bv.size() > 2 || outputs.empty() || outputs.size() > kMaxOutputs) {
            return E_INVALIDARG;
        }
        auto &p = *impl_;
        p.skip = Skip::none;
        if (p.diagnostic_decisions) p.poll_decisions();
        const auto skip = [&](Skip reason) {
            reset();
            p.skip = reason;
            return S_FALSE;
        };
        if (p.capture_only) {
            return skip(Skip::guides);
        }
        if (outputs.size() > p.max_outputs) {
            return skip(Skip::multi_frame_unsupported);
        }
        if (!ag || !bg || ag->eye_count != bg->eye_count || !bg->eye_count ||
            bg->eye_count > 2) {
            return skip(Skip::guides);
        }
        // Validate the entire stereo pair before writing a command. No partial
        // eye, guessed depth, or cross-stream history is handed to NGX.
        const auto guide_index = [&](UINT i) {
            return p.source.DepthOrArraySize == 1 && bg->eye_count == bv.size()
                ? i : slice(bv[i], i, UINT(bv.size()), p.source.DepthOrArraySize);
        };
        for (UINT i = 0; i < bv.size(); ++i) {
            const UINT sl = guide_index(i);
            if (sl >= bg->eye_count || !ag->eyes[sl] || !bg->eyes[sl]) {
                return skip(Skip::guides);
            }
            const auto &ga = *ag->eyes[sl];
            const auto &gb = *bg->eyes[sl];
            if (gb.reset || !ga.serial || gb.previous_serial != ga.serial ||
                ga.stream != gb.stream || ga.epoch != gb.epoch) {
                return skip(Skip::guides);
            }
            if (ga.depth_inverted != gb.depth_inverted || ga.depth_infinite != gb.depth_infinite ||
                ga.camera_near != gb.camera_near || ga.camera_far != gb.camera_far) {
                return skip(Skip::guides);
            }
            // The aligned A is written into textures sized for B's viewport.
            const auto ar = rect(av[i], UINT(p.source.Width), p.source.Height);
            const auto br = rect(bv[i], UINT(p.source.Width), p.source.Height);
            if (ar.width != br.width || ar.height != br.height) {
                return skip(Skip::guides);
            }
            for (const auto *g : {&ga, &gb}) {
                const auto r = g == &ga ? ar : br;
                const auto mf = g->motion_vectors
                                    ? motion_format(g->motion_vectors->GetDesc().Format)
                                    : DXGI_FORMAT_UNKNOWN;
                const auto df =
                    g->depth ? depth_format(g->depth->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;
                if (!same_device(p.device.Get(), g->motion_vectors.Get()) ||
                    !same_device(p.device.Get(), g->depth.Get()) ||
                    !valid_rect(g->motion_vectors.Get(), g->motion_x, g->motion_y, g->motion_width,
                                g->motion_height, g->motion_slice) ||
                    !valid_rect(
                        g->depth.Get(), g->depth_x, g->depth_y, g->depth_width, g->depth_height,
                        g->depth && g->depth->GetDesc().DepthOrArraySize == 1 ? 0
                                                                              : g->output_slice) ||
                    (mf != DXGI_FORMAT_R16G16_FLOAT && mf != DXGI_FORMAT_R32G32_FLOAT) ||
                    (df != DXGI_FORMAT_R32_FLOAT && df != DXGI_FORMAT_R16_UNORM &&
                     df != DXGI_FORMAT_R24_UNORM_X8_TYPELESS &&
                     df != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) ||
                    !std::isfinite(g->camera_near) || g->camera_near <= 0 ||
                    !std::isfinite(g->camera_far) || g->camera_far <= g->camera_near ||
                    !std::isfinite(g->scale_x) || !std::isfinite(g->scale_y) ||
                    !std::isfinite(g->jitter_x) || !std::isfinite(g->jitter_y) ||
                    !std::isfinite(g->previous_jitter_x) || !std::isfinite(g->previous_jitter_y) ||
                    !g->output_width || !g->output_height || r.offset_x < g->output_x ||
                    r.offset_y < g->output_y ||
                    std::uint64_t(r.offset_x) + r.width >
                        std::uint64_t(g->output_x) + g->output_width ||
                    std::uint64_t(r.offset_y) + r.height >
                        std::uint64_t(g->output_y) + g->output_height) {
                    return skip(Skip::guides);
                }
            }
        }
        // Changing viewport dimensions destroys neural history. A previous
        // queue submission can still own it: poll its fence, never CPU-wait.
        for (UINT i = 0; i < bv.size(); ++i) {
            const auto r = rect(bv[i], UINT(p.source.Width), p.source.Height);
            const auto &e = p.eyes[i];
            if (e.handle && (e.width != r.width || e.height != r.height) && p.completion &&
                p.completion->GetCompletedValue() < p.completion_value) {
                return skip(Skip::resizing);
            }
        }
        // A feature NGX refused is retried after a pause rather than every
        // pair; the pairs in between show the current frame.
        if (p.create_retry) {
            --p.create_retry;
            return skip(Skip::feature_unavailable);
        }
        for (UINT i = 0; i < bv.size(); ++i) {
            const auto r = rect(bv[i], UINT(p.source.Width), p.source.Height);
            auto &e = p.eyes[i];
            HRESULT hr = S_OK;
            const bool creating = !e.handle || e.width != r.width || e.height != r.height;
            if (creating) {
                hr = p.create_eye(list, e, r.width, r.height);
            }
            if (SUCCEEDED(hr)) {
                hr = p.ensure_outputs(e, UINT(outputs.size()));
            }
            if (FAILED(hr)) {
                p.report("feature creation failed", unsigned(hr));
                // An existing eye may still be in use by a submitted pair;
                // only a feature that failed to come up is torn down.
                if (creating) p.release_eye(e);
                p.create_retry = kCreateRetryPairs;
                return skip(Skip::feature_unavailable);
            }
        }
        Transitions t(list);
        t.to(a, kCommon, kAnyRead);
        t.to(b, kCommon, kAnyRead);
        std::array<Params, 2> eye_params{};
        for (UINT i = 0; i < bv.size(); ++i) {
            auto &e = p.eyes[i];
            const UINT sl = guide_index(i);
            const auto &ga = *ag->eyes[sl];
            const auto &gb = *bg->eyes[sl];
            const UINT base = i * kBlocksPerEye * kBlock;
            // Align A and the engine backward motion into B's camera plane.
            // The NGX outputs can then keep the existing OpenXR B pose/FOV
            // contract, instead of presenting midpoint-camera pixels as B.
            const auto set_mapping = [&](Params &v) {
                XMStoreFloat4(reinterpret_cast<XMFLOAT4 *>(v.rotation), b_to_a(av[i], bv[i]));
                const auto at = tangents(av[i].fov), bt = tangents(bv[i].fov);
                std::copy(at.begin(), at.end(), v.source_tangents);
                std::copy(bt.begin(), bt.end(), v.target_tangents);
            };
            auto &params = eye_params[i];
            params = p.parameters(bv[i], gb, i, UINT(bv.size()));
            set_mapping(params);
            const bool continuous = e.serial == ga.serial && e.stream == ga.stream &&
                                    e.epoch == ga.epoch;
            if (!continuous ||
                alignment_pixels(av[i], bv[i], params.extent[0], params.extent[1]) >
                    kReseedPixels) {
                auto pa = p.parameters(av[i], ga, i, UINT(av.size()));
                set_mapping(pa);
                pa.pack_previous = 1;
                p.pack_input(list, t, slot, base, e, a, ga, pa, b);
                if (FAILED(p.evaluate(list, t, e, bv[i], ga, pa, 1, 1, true))) {
                    t.restore();
                    return skip(Skip::evaluate_failed);
                }
                t.uav(e.generated[0].Get());
                t.uav(e.disable[0].Get());
                ++p.reseeds;
            }
            p.pack_input(list, t, slot, base + kBlock, e, b, gb, params, b);
            for (UINT output = 0; output < outputs.size(); ++output) {
                if (FAILED(p.evaluate(list, t, e, bv[i], gb, params, UINT(outputs.size()),
                                      output + 1, false))) {
                    t.restore();
                    return skip(Skip::evaluate_failed);
                }
            }
        }
        // Every evaluation succeeded; only now are the outputs written.
        for (UINT i = 0; i < bv.size(); ++i) {
            for (UINT output = 0; output < outputs.size(); ++output) {
                t.to(p.eyes[i].generated[output].Get(), kCommon, kPixelRead);
                t.to(p.eyes[i].disable[output].Get(), kCommon, kPixelRead);
            }
        }
        for (const auto &output : outputs) {
            t.to(output.image, release, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        t.flush();
        ID3D12DescriptorHeap *hh[]{p.heaps[slot].Get()};
        list->SetDescriptorHeaps(1, hh);
        list->SetGraphicsRootSignature(p.root.Get());
        list->SetPipelineState(p.compose.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT output = 0; output < outputs.size(); ++output) {
            for (UINT i = 0; i < bv.size(); ++i) {
                auto &e = p.eyes[i];
                const auto &gb = *bg->eyes[guide_index(i)];
                const auto &params = eye_params[i];
                const UINT outbase = i * kBlocksPerEye * kBlock + (output + 2) * kBlock;
                p.descriptors(slot, outbase, e, b, gb, output, b);
                list->SetGraphicsRootDescriptorTable(0, p.gpu(slot, outbase));
                list->SetGraphicsRootDescriptorTable(1, p.gpu(slot, outbase + 6));
                list->SetGraphicsRoot32BitConstants(2, 40, &params, 0);
                const auto rh = p.target(slot, i * kMaxOutputs + output, outputs[output].image,
                                         params.color_slice);
                list->OMSetRenderTargets(1, &rh, FALSE, nullptr);
                const D3D12_VIEWPORT vp{params.output_rect[0],
                                        params.output_rect[1],
                                        float(params.extent[0]),
                                        float(params.extent[1]),
                                        0,
                                        1};
                const D3D12_RECT sc{LONG(vp.TopLeftX), LONG(vp.TopLeftY),
                                    LONG(vp.TopLeftX + vp.Width), LONG(vp.TopLeftY + vp.Height)};
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &sc);
                list->DrawInstanced(3, 1, 0, 0);
            }
        }
        if (p.diagnostic_decisions) {
            for (UINT i = 0; i < bv.size(); ++i) {
                for (UINT output = 0; output < outputs.size(); ++output) {
                    t.to(p.eyes[i].disable[output].Get(), kCommon,
                         D3D12_RESOURCE_STATE_COPY_SOURCE);
                }
            }
            t.flush();
            for (UINT i = 0; i < bv.size(); ++i) {
                for (UINT output = 0; output < outputs.size(); ++output) {
                    list->CopyBufferRegion(p.decisions[slot].buffer.Get(),
                        (i * 2 + output) * sizeof(UINT), p.eyes[i].disable[output].Get(), 0,
                        sizeof(UINT));
                }
            }
            p.recorded_slot = slot;
            auto &readback = p.decisions[slot];
            readback.publication = bg->eyes[0]->publication;
            readback.eyes = UINT(bv.size());
            readback.outputs = UINT(outputs.size());
        }
        t.restore();
        for (UINT i = 0; i < bv.size(); ++i) {
            const auto &gb = *bg->eyes[guide_index(i)];
            p.eyes[i].serial = gb.serial;
            p.eyes[i].stream = gb.stream;
            p.eyes[i].epoch = gb.epoch;
        }
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
#else
    (void)list;
    (void)slot;
    (void)a;
    (void)b;
    (void)av;
    (void)bv;
    (void)ag;
    (void)bg;
    (void)outputs;
    (void)release;
    return DXGI_ERROR_UNSUPPORTED;
#endif
}

std::uint32_t native_dlssg_max_generated_frames(ID3D12Device *device) noexcept {
#ifdef XRFG_NATIVE_DLSSG
    try {
        if (!device) {
            return 0;
        }
        const LUID adapter = device->GetAdapterLuid();
        {
            std::scoped_lock lock(ngx_mutex);
            for (const auto &e : ngx_frames) {
                if (same_adapter(e.adapter, adapter)) {
                    return e.frames;
                }
            }
        }
        if (FAILED(acquire_ngx(device))) {
            return 0;
        }
        const std::uint32_t frames = query_generated_frames();
        release_ngx(device);
        remember_frames(device, frames);
        return frames;
    } catch (...) {
        return 0;
    }
#else
    (void)device;
    return 0;
#endif
}
} // namespace xrfg
