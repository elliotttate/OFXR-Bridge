#include "xrfg/ngx_guide_capture.hpp"
#ifdef XRFG_NATIVE_DLSSG
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/third_party/uevr_api.h"
#include <windows.h>
#include <wrl/client.h>
#include <nvsdk_ngx_params.h>
#include <safetyhook.hpp>
#include <d3d12sdklayers.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <share.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xrfg { namespace {
using Microsoft::WRL::ComPtr;
using Create = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using Evaluate = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, void*);
// flags_known is false for a feature recovered from its evaluate parameters
// when NGX had already created it before the hook was installed.
struct Feature { int flags{}; UINT width{},height{},out_width{},out_height{}; bool flags_known{true}; };
struct Module {
    SafetyHookInline create_hook, evaluate_hook;
    Create create{}; Evaluate evaluate{};
    HMODULE retained{};
};
struct Capture {
    std::mutex mutex, log_mutex;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<Module,2> modules;
    std::unordered_map<const NVSDK_NGX_Handle*,Feature> features;
    std::atomic_bool active{false}, started{false}, camera_ready{false};
    std::atomic_uint64_t calls{0};
    FILE* log{};
    ComPtr<ID3D12InfoQueue1> debug_queue;
    DWORD debug_cookie{};
    float camera_near{}, camera_far{};
    bool depth_inverted{}, depth_infinite{}, camera_valid{}, camera_from_xr{};
};
// Hooks retain their trampolines and pin this module for process lifetime.
// No CRT static destructor tries to unhook under the Windows loader lock.
Capture& state() { static auto* s=new Capture; return *s; }
void log(const char* message, UINT64 a=0, UINT64 b=0, UINT64 c=0, UINT64 d=0) {
    auto& s=state(); std::scoped_lock lock(s.log_mutex);
    if(!s.log) {
        wchar_t dir[MAX_PATH]{}; GetModuleFileNameW(
            reinterpret_cast<HMODULE>([] { HMODULE m{}; GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&state),&m); return m; }()),dir,MAX_PATH);
        auto* slash=wcsrchr(dir,L'\\'); if(!slash)return; *slash=0;
        wchar_t path[MAX_PATH]{}; swprintf_s(path,L"%s\\ofxr-ngx-capture-pid%lu.log",dir,GetCurrentProcessId());
        s.log=_wfsopen(path,L"wb",_SH_DENYWR);
    }
    if(s.log) { fprintf(s.log,"%llu %s a=%llu b=%llu c=%llu d=%llu\n",
        GetTickCount64(),message,a,b,c,d); fflush(s.log); }
}
template<class T> T get(const NVSDK_NGX_Parameter* p,const char* name,T fallback={}) {
    T value=fallback;
    if(NVSDK_NGX_FAILED(p->Get(name,&value)))return fallback;
    return value;
}
template<class T> ComPtr<T> native(T* object) {
    ComPtr<T> result=object;
    if(!object)return result;
    using Unwrap=int(*)(void*,void**);
    if(auto mod=GetModuleHandleW(L"sl.interposer.dll")) {
        auto unwrap=reinterpret_cast<Unwrap>(GetProcAddress(mod,"slGetNativeInterface"));
        void* raw=nullptr;
        if(unwrap && unwrap(object,&raw)==0 && raw) result=static_cast<T*>(raw);
    }
    return result;
}
bool same(IUnknown* a,IUnknown* b) {
    ComPtr<IUnknown> x,y;
    return a&&b&&SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x)))&&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y)))&&x==y;
}
void update_uevr_projection() {
    // UEVR's public SDK exposes the projection actually used by the renderer.
    // This also works when the application does not attach OpenXR depth metadata.
    auto module=GetModuleHandleW(L"UEVRBackend.dll");
    if(!module)return;
    auto* api=reinterpret_cast<const UEVR_PluginInitializeParam*>(
        GetProcAddress(module,"g_plugin_initialize_param"));
    if(!api||!api->version||api->version->major!=2||!api->vr||
       !api->vr->is_runtime_ready||!api->vr->is_runtime_ready()||
       !api->vr->get_ue_projection_matrix)return;
    UEVR_Matrix4x4f eyes[2]{};
    api->vr->get_ue_projection_matrix(0,&eyes[0]);
    api->vr->get_ue_projection_matrix(1,&eyes[1]);
    for(const auto& eye:eyes) {
        for(const auto& row:eye.m)for(float value:row)if(!std::isfinite(value))return;
        // Accept only a validated reversed, infinite perspective projection.
        if(eye.m[2][2]!=0.F||eye.m[2][3]!=1.F||eye.m[3][3]!=0.F||
           eye.m[3][2]<=0.F||eye.m[0][0]<=0.F||eye.m[1][1]<=0.F)return;
    }
    if(eyes[0].m[3][2]!=eyes[1].m[3][2])return;
    auto& s=state();std::scoped_lock lock(s.mutex);
    if(s.camera_from_xr)return;
    const bool first=!s.camera_valid;
    // Keep renderer units throughout the depth/projection math. No assumed UE
    // world-to-meter constant is needed for the rotational camera alignment.
    s.camera_near=eyes[0].m[3][2];
    s.camera_far=s.camera_near*10000.F; // unused by infinite-Z unprojection
    s.depth_inverted=true;s.depth_infinite=true;s.camera_valid=true;s.camera_ready=true;
    if(first)log("uevr_projection",UINT64(s.camera_near*1000000.F),api->version->minor);
}
void capture(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* p) {
    auto& s=state(); if(!s.active.load())return;
    const auto call=++s.calls;
    if(!list||!p)return;
    // The projection rarely changes: refresh it every 32 evaluations, and on
    // every one until it is known. OpenXR depth metadata takes precedence.
    if(!s.camera_ready.load()||call%32==0)update_uevr_projection();
    ComPtr<ID3D12CommandQueue> queue; Feature feature{}; bool known{};
    float camera_near{},camera_far{}; bool inverted{},infinite{},camera_valid{};
    { std::scoped_lock lock(s.mutex); queue=s.queue;
      auto it=s.features.find(handle); if(it!=s.features.end()){feature=it->second;known=true;}
      camera_near=s.camera_near;camera_far=s.camera_far;
      inverted=s.depth_inverted;infinite=s.depth_infinite;camera_valid=s.camera_valid; }
    if(!queue)return;
    if(!known) {
        // A feature created before the hook was installed: the game's
        // parameter map normally still holds its creation values.
        int flags{}; feature.flags_known=NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&flags));
        feature.flags=feature.flags_known?flags:0;
        feature.width=get<UINT>(p,NVSDK_NGX_Parameter_Width); feature.height=get<UINT>(p,NVSDK_NGX_Parameter_Height);
        feature.out_width=get<UINT>(p,NVSDK_NGX_Parameter_OutWidth); feature.out_height=get<UINT>(p,NVSDK_NGX_Parameter_OutHeight);
        {std::scoped_lock lock(s.mutex); s.features.emplace(handle,feature);}
        log(feature.flags_known?"feature_recovered":"feature_recovered_without_flags",
            reinterpret_cast<UINT64>(handle),feature.flags,(UINT64(feature.width)<<32)|feature.height);
    }
    auto* output=get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_Output);
    auto motion=native(get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_MotionVectors));
    auto depth=native(get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_Depth));
    if(!output||!motion||!depth) { if(call<8)log("missing_guides",call); return; }
    auto cmd=native(list); auto nq=native(queue.Get());
    ComPtr<ID3D12Device> md,dd,cd,qd;
    if(FAILED(motion->GetDevice(IID_PPV_ARGS(&md)))||FAILED(depth->GetDevice(IID_PPV_ARGS(&dd)))||
       FAILED(cmd->GetDevice(IID_PPV_ARGS(&cd)))||FAILED(nq->GetDevice(IID_PPV_ARGS(&qd)))||
       !same(md.Get(),dd.Get())||!same(md.Get(),cd.Get())||!same(md.Get(),qd.Get())) {
        if(call<8)log("device_mismatch",call); return;
    }
    const auto od=output->GetDesc(), mv=motion->GetDesc(), dz=depth->GetDesc();
    DlssMotionVectorPublication g{};
    g.stream=reinterpret_cast<UINT64>(handle); g.output=output;
    g.motion_vectors=motion.Get(); g.depth=depth.Get();
    g.producer_queue=queue.Get(); g.producer_command_list=cmd.Get(); g.verified_producer_device=md.Get();
    g.output_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);
    g.output_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);
    g.output_width=feature.out_width?feature.out_width:get<UINT>(p,NVSDK_NGX_Parameter_OutWidth,UINT(od.Width));
    g.output_height=feature.out_height?feature.out_height:get<UINT>(p,NVSDK_NGX_Parameter_OutHeight,od.Height);
    g.motion_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X);
    g.motion_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
    const auto rw=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,feature.width);
    const auto rh=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,feature.height);
    // Without creation flags, assume render-resolution vectors: the usual
    // DLSS integration, and the one whose rectangle the render size gives.
    const bool full_mv=feature.flags_known&&(feature.flags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)==0;
    g.motion_width=full_mv?g.output_width:(rw?rw:UINT(mv.Width));
    g.motion_height=full_mv?g.output_height:(rh?rh:mv.Height);
    g.depth_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);
    g.depth_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
    g.depth_width=rw?rw:UINT(dz.Width); g.depth_height=rh?rh:dz.Height;
    g.scale_x=get<float>(p,NVSDK_NGX_Parameter_MV_Scale_X,1);
    g.scale_y=get<float>(p,NVSDK_NGX_Parameter_MV_Scale_Y,1);
    g.jitter_x=get<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_X);
    g.jitter_y=get<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_Y);
    g.jittered=(feature.flags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0;
    g.reset=get<int>(p,NVSDK_NGX_Parameter_Reset)!=0;
    g.resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    g.depth_resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    g.frame_time_delta_ms=get<float>(p,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,16.6667F);
    g.camera_near=get<float>(p,"DLSSG.CameraNear",camera_near);
    g.camera_far=get<float>(p,"DLSSG.CameraFar",camera_far);
    // Unknown flags defer to the camera's own depth convention.
    g.depth_inverted=feature.flags_known?(feature.flags&NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)!=0:inverted;
    // A game with no camera metadata - a native VR game that submits no OpenXR
    // depth, say - still states its depth direction in the feature's flags,
    // which its own DLSS relies on. That is all NGX's result depends on: it
    // measured bit-identical for near planes from 0.01 to 100, finite or
    // infinite, while a wrong direction quadrupled the error at moving edges.
    // So nominal planes stand in for the camera's.
    if(!camera_valid&&feature.flags_known) {
        camera_valid=true; inverted=g.depth_inverted; infinite=false;
        g.camera_near=get<float>(p,"DLSSG.CameraNear",0.1F);
        g.camera_far=get<float>(p,"DLSSG.CameraFar",1000.F);
    }
    if(!camera_valid||inverted!=g.depth_inverted) {
        // The motion alone still serves OFXR + DLSS vectors; native generation
        // finds no depth and shows the current frame.
        if(call<=8||call%300==0)log("waiting_depth_convention",call,feature.flags,camera_valid,inverted);
        g.depth=nullptr;
    }
    g.depth_infinite=infinite;
    publish_dlss_motion_vectors(g);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::dlss_evaluation, 0,
        g.stream, call, g.output_x);
    const HRESULT removed=md->GetDeviceRemovedReason();
    if(FAILED(removed))log("device_removed",static_cast<UINT>(removed),call);
    if(call<=8||call%300==0) {
        log("capture",call,g.stream,(UINT64(g.output_width)<<32)|g.output_height,feature.flags);
        log("rects",(UINT64(g.output_x)<<32)|g.output_y,(UINT64(g.motion_x)<<32)|g.motion_y,
            (UINT64(g.motion_width)<<32)|g.motion_height,(UINT64(g.depth_width)<<32)|g.depth_height);
        log("formats",mv.Format,dz.Format,od.Width,od.Height);
    }
}
template<UINT slot> NVSDK_NGX_Result create(ID3D12GraphicsCommandList* list,NVSDK_NGX_Feature id,
    NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out) {
    const auto result=state().modules[slot].create(list,id,p,out);
    if(NVSDK_NGX_SUCCEED(result)&&out&&*out&&p&&
       (id==NVSDK_NGX_Feature_SuperSampling||id==NVSDK_NGX_Feature_RayReconstruction)) {
        Feature f{get<int>(p,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags),
            get<UINT>(p,NVSDK_NGX_Parameter_Width),get<UINT>(p,NVSDK_NGX_Parameter_Height),
            get<UINT>(p,NVSDK_NGX_Parameter_OutWidth),get<UINT>(p,NVSDK_NGX_Parameter_OutHeight)};
        {std::scoped_lock lock(state().mutex); state().features[*out]=f;}
        log("feature",reinterpret_cast<UINT64>(*out),f.flags,(UINT64(f.width)<<32)|f.height,
            (UINT64(f.out_width)<<32)|f.out_height);
    }
    return result;
}
template<UINT slot> NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter* p,void* callback) {
    const auto result=state().modules[slot].evaluate(list,h,p,callback);
    if(NVSDK_NGX_SUCCEED(result)) { try { capture(list,h,p); } catch(...) {log("capture_exception");} }
    return result;
}
void install(UINT i,const wchar_t* name,void* create_fn,void* eval_fn) {
    auto& m=state().modules[i]; if(m.retained)return;
    auto mod=GetModuleHandleW(name); if(!mod)return;
    auto c=GetProcAddress(mod,"NVSDK_NGX_D3D12_CreateFeature");
    auto e=GetProcAddress(mod,"NVSDK_NGX_D3D12_EvaluateFeature"); if(!c||!e)return;
    HMODULE retained{}; if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(e),&retained))return;
    m.create_hook=safetyhook::create_inline(c,create_fn,SafetyHookInline::StartDisabled);
    m.evaluate_hook=safetyhook::create_inline(e,eval_fn,SafetyHookInline::StartDisabled);
    if(!m.create_hook||!m.evaluate_hook){FreeLibrary(retained);log("hook_failed",i);return;}
    m.create=m.create_hook.original<Create>(); m.evaluate=m.evaluate_hook.original<Evaluate>();
    m.retained=retained; (void)m.create_hook.enable(); (void)m.evaluate_hook.enable(); log("hook_installed",i);
}
// NGX looks for a feature's module only along the path list of the process's
// first initialisation, which is usually the game's own DLSS: OFXR's later
// one succeeds yet reports frame generation not found when the game ships no
// nvngx_dlssg.dll. OFXR's directory goes last on every initialisation's list,
// so a game's own copy still wins. Both core entry points take the list as
// their last argument; the SDK's static library routes every D3D12
// initialisation through one of them.
using InitExt=NVSDK_NGX_Result(*)(unsigned long long,const wchar_t*,ID3D12Device*,
    NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
using InitProject=NVSDK_NGX_Result(*)(const char*,NVSDK_NGX_EngineType,const char*,
    const wchar_t*,ID3D12Device*,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
struct Discovery {
    SafetyHookInline ext_hook, project_hook;
    InitExt ext{}; InitProject project{};
    std::wstring directory;
};
Discovery& discovery() { static auto* d=new Discovery; return *d; }
struct DiscoveryInfo {
    NVSDK_NGX_FeatureCommonInfo info{};
    std::vector<const wchar_t*> paths;
    const NVSDK_NGX_FeatureCommonInfo* with_directory(const NVSDK_NGX_FeatureCommonInfo* given) {
        const auto& directory=discovery().directory;
        if(given)info=*given;
        const UINT count=given&&given->PathListInfo.Path?given->PathListInfo.Length:0;
        for(UINT i=0;i<count;++i) {
            const wchar_t* path=given->PathListInfo.Path[i];
            if(path&&_wcsicmp(path,directory.c_str())==0)return given;
            paths.push_back(path);
        }
        paths.push_back(directory.c_str());
        info.PathListInfo.Path=paths.data();
        info.PathListInfo.Length=static_cast<unsigned int>(paths.size());
        log("ngx_discovery_paths",paths.size());
        return &info;
    }
};
NVSDK_NGX_Result init_ext(unsigned long long id,const wchar_t* data,ID3D12Device* device,
    NVSDK_NGX_Version version,const NVSDK_NGX_FeatureCommonInfo* given) {
    DiscoveryInfo extended; const NVSDK_NGX_FeatureCommonInfo* info=given;
    try { info=extended.with_directory(given); } catch(...) {}
    return discovery().ext(id,data,device,version,info);
}
NVSDK_NGX_Result init_project(const char* project,NVSDK_NGX_EngineType engine,
    const char* engine_version,const wchar_t* data,ID3D12Device* device,
    NVSDK_NGX_Version version,const NVSDK_NGX_FeatureCommonInfo* given) {
    DiscoveryInfo extended; const NVSDK_NGX_FeatureCommonInfo* info=given;
    try { info=extended.with_directory(given); } catch(...) {}
    return discovery().project(project,engine,engine_version,data,device,version,info);
}
// The driver's NGX core, as the SDK finds it when nothing ships next to the
// game.
HMODULE load_ngx_core() {
    if(auto core=GetModuleHandleW(L"_nvngx.dll")) { log("ngx_core_already_loaded"); return core; }
    wchar_t directory[MAX_PATH]{}; DWORD size=sizeof(directory);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
        L"FullPath",RRF_RT_REG_SZ,nullptr,directory,&size)!=ERROR_SUCCESS)return nullptr;
    std::wstring path=directory; path+=L"\\_nvngx.dll";
    return LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
}
} // namespace
void prepare_ngx_feature_discovery(const wchar_t* directory) noexcept {
    try {
        auto& d=discovery(); if(d.ext_hook||d.project_hook||!directory||!*directory)return;
        const HMODULE core=load_ngx_core(); if(!core)return;
        auto ext=GetProcAddress(core,"NVSDK_NGX_D3D12_Init_Ext");
        auto project=GetProcAddress(core,"NVSDK_NGX_D3D12_Init_ProjectID");
        if(!ext||!project)return;
        HMODULE pin{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&prepare_ngx_feature_discovery),&pin);
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(ext),&pin);
        d.directory=directory;
        d.ext_hook=safetyhook::create_inline(ext,reinterpret_cast<void*>(&init_ext),
            SafetyHookInline::StartDisabled);
        d.project_hook=safetyhook::create_inline(project,reinterpret_cast<void*>(&init_project),
            SafetyHookInline::StartDisabled);
        if(!d.ext_hook||!d.project_hook){d.ext_hook={};d.project_hook={};log("ngx_discovery_hook_failed");return;}
        d.ext=d.ext_hook.original<InitExt>(); d.project=d.project_hook.original<InitProject>();
        (void)d.ext_hook.enable(); (void)d.project_hook.enable(); log("ngx_discovery_hooked");
    } catch(...) {}
}
void configure_ngx_guide_capture(ID3D12CommandQueue* queue,bool enabled) noexcept {
    try {
        auto& s=state(); {std::scoped_lock lock(s.mutex);
            if(s.queue.Get()!=queue){s.camera_valid=false;s.camera_from_xr=false;s.camera_ready=false;}
            s.queue=queue;}
        if(queue&&!s.debug_queue) {
            auto nq=native(queue);ComPtr<ID3D12Device> device;
            if(SUCCEEDED(nq->GetDevice(IID_PPV_ARGS(&device)))&&
               SUCCEEDED(device.As(&s.debug_queue))) {
                s.debug_queue->RegisterMessageCallback(
                    [](D3D12_MESSAGE_CATEGORY category,D3D12_MESSAGE_SEVERITY severity,
                       D3D12_MESSAGE_ID id,LPCSTR description,void*) {
                        if(severity<=D3D12_MESSAGE_SEVERITY_WARNING&&id!=1424)log(description,id,category,severity);
                    },D3D12_MESSAGE_CALLBACK_FLAG_NONE,nullptr,&s.debug_cookie);
                log("debug_messages_enabled");
            }
        }
        s.active=enabled&&queue;
        if(enabled&&queue)configure_dlss_motion_vector_tracking(true);
        if(!s.active||s.started.exchange(true))return;
        HMODULE pin{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&configure_ngx_guide_capture),&pin);
        if(GetEnvironmentVariableW(L"XRFG_TEST_NATIVE_DLSSG_DRED",nullptr,0)) {
            auto nq=native(queue);ComPtr<ID3D12Device> device;
            if(SUCCEEDED(nq->GetDevice(IID_PPV_ARGS(&device)))) {
                std::thread([device] {
                    for(;;) {
                        const HRESULT reason=device->GetDeviceRemovedReason();
                        if(FAILED(reason)) {
                            log("device_removed_dred",static_cast<UINT>(reason));
                            ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
                            if(SUCCEEDED(device.As(&dred))) {
                                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 data{};
                                if(SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&data))) {
                                    for(auto* node=data.pHeadAutoBreadcrumbNode;node;node=node->pNext) {
                                        const auto done=node->pLastBreadcrumbValue?*node->pLastBreadcrumbValue:0;
                                        if(done<node->BreadcrumbCount)log(node->pCommandListDebugNameA?
                                            node->pCommandListDebugNameA:"dred_list",done,node->BreadcrumbCount,
                                            done<node->BreadcrumbCount?node->pCommandHistory[done]:0);
                                    }
                                }
                            }
                            return;
                        }
                        Sleep(1);
                    }
                }).detach();
            }
        }
        // NGX loads a feature module inside the game's first CreateFeature,
        // so that creation can precede the hook; capture() recovers it.
        std::thread([] {for(;;) {
            if(state().active) {
                install(0,L"nvngx_dlss.dll",reinterpret_cast<void*>(&create<0>),reinterpret_cast<void*>(&evaluate<0>));
                install(1,L"nvngx_dlssd.dll",reinterpret_cast<void*>(&create<1>),reinterpret_cast<void*>(&evaluate<1>));
                if(state().modules[0].retained&&state().modules[1].retained)return;
            }
            Sleep(500);
        }}).detach(); log("enabled");
    } catch(...) {}
}
void stop_ngx_guide_capture(ID3D12CommandQueue* queue) noexcept {
    auto& s=state(); std::scoped_lock lock(s.mutex);
    if(s.queue.Get()==queue) {s.active=false;s.queue.Reset();}
}
void update_ngx_guide_depth(float near_z,float far_z,float min_depth,float max_depth) noexcept {
    if(min_depth!=0.F||max_depth!=1.F||!std::isfinite(near_z)||!std::isfinite(far_z)||
        near_z<=0||far_z<=0||near_z==far_z)return;
    auto& s=state();std::scoped_lock lock(s.mutex);
    s.depth_inverted=near_z>far_z;
    s.camera_near=std::min(near_z,far_z);
    s.camera_far=std::max(near_z,far_z);
    s.depth_infinite=s.camera_far>1e20F;
    if(s.depth_infinite)s.camera_far=1000.F;
    s.camera_valid=true;
    s.camera_from_xr=true;
    s.camera_ready=true;
}
} // namespace xrfg
#else
namespace xrfg {
void configure_ngx_guide_capture(ID3D12CommandQueue*,bool) noexcept {}
void stop_ngx_guide_capture(ID3D12CommandQueue*) noexcept {}
void update_ngx_guide_depth(float,float,float,float) noexcept {}
void prepare_ngx_feature_discovery(const wchar_t*) noexcept {}
}
#endif
