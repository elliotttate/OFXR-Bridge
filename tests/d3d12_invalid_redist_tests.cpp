// A host exe whose Agility SDK export names a folder that is not there, as
// Beat Saber's does: every D3D12CreateDevice in the process then fails with
// D3D12_ERROR_INVALID_REDIST, including the D3D11 bridge's own device. The
// bridge falls back to the system's D3D12 through a device factory.
#include "xrfg/d3d11_d3d12_interop.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 715;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\no-such-agility-sdk\\";
}

int main() {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return 1;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) == S_OK; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) break;
    }
    if (!adapter) return 1;

    // The scenario: the host's configuration refuses a plain device.
    ComPtr<ID3D12Device> plain;
    const HRESULT plain_result = D3D12CreateDevice(
        adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(plain.GetAddressOf()));
    std::printf("D3D12CreateDevice: 0x%08lx\n", static_cast<unsigned long>(plain_result));
    if (plain_result != static_cast<HRESULT>(0x887E0003L)) {
        std::printf("the host configuration did not reproduce the invalid redist\n");
        return 1;
    }

    // The bridge's device, on a D3D11 device of the same adapter.
    ComPtr<ID3D11Device> d3d11;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, d3d11.GetAddressOf(), nullptr, nullptr))) {
        return 1;
    }
    ComPtr<ID3D12Device> bridge_device;
    ComPtr<ID3D12CommandQueue> bridge_queue;
    const HRESULT bridge_result = xrfg::create_d3d12_device_for_d3d11(
        d3d11.Get(), bridge_device.GetAddressOf(), bridge_queue.GetAddressOf());
    std::printf("create_d3d12_device_for_d3d11: 0x%08lx\n", static_cast<unsigned long>(bridge_result));
    if (FAILED(bridge_result) || !bridge_device || !bridge_queue) {
        std::printf("the bridge found no device under an invalid redist\n");
        return 1;
    }
    // Root signatures are refused the same way, and serialize on the device.
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 1;
    description.pParameters = &parameter;
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    const HRESULT serialize_result = xrfg::serialize_root_signature(
        bridge_device.Get(), description, blob.GetAddressOf(), error.GetAddressOf());
    ComPtr<ID3D12RootSignature> root_signature;
    if (FAILED(serialize_result) || !blob ||
        FAILED(bridge_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                  IID_PPV_ARGS(root_signature.GetAddressOf())))) {
        std::printf("no root signature under an invalid redist: 0x%08lx\n",
                    static_cast<unsigned long>(serialize_result));
        return 1;
    }
    if (xrfg::global_d3d12_functions_usable()) {
        std::printf("the global D3D12 functions were reported usable\n");
        return 1;
    }
    std::printf("invalid redist tests passed\n");
    return 0;
}
