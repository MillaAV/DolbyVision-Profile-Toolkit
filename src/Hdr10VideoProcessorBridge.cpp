#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <cmath>
#include <algorithm>
#include "MinHook.h"

extern "C" void* P7Hdr10GetStreamSetTarget(void* videoContext);

using DisplayPeakFn = float (*)();
using Hdr10ActiveFn = bool (*)();
using LogFn = void (*)(const wchar_t*, ...);
using CreateDeviceFn = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDeviceAndSwapChainFn = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using SetStreamHdrFn = void (STDMETHODCALLTYPE*)(ID3D11VideoContext2*, ID3D11VideoProcessor*,
    UINT, DXGI_HDR_METADATA_TYPE, UINT, const void*);

static DisplayPeakFn g_peak = nullptr;
static Hdr10ActiveFn g_hdr10 = nullptr;
static LogFn g_log = nullptr;
static CreateDeviceFn g_createDevice = nullptr;
static CreateDeviceAndSwapChainFn g_createDeviceAndSwapChain = nullptr;
static SRWLOCK g_bridgeLock = SRWLOCK_INIT;
static volatile LONG g_metadataCalls = 0;
static volatile LONG g_metadataAdjustments = 0;

struct HdrTarget {
    void* target = nullptr;
    SetStreamHdrFn original = nullptr;
};
static HdrTarget g_targets[16]{};
static UINT g_targetCount = 0;

static bool IsDisabled() {
    return GetFileAttributesW(L"C:\\ProgramData\\MillaAV\\P7DV\\hdr10-adaptive.off") != INVALID_FILE_ATTRIBUTES;
}

static void STDMETHODCALLTYPE SetHdrStreamHook(
    ID3D11VideoContext2* self,
    ID3D11VideoProcessor* processor,
    UINT streamIndex,
    DXGI_HDR_METADATA_TYPE type,
    UINT size,
    const void* data) {

    SetStreamHdrFn original = nullptr;
    void* target = P7Hdr10GetStreamSetTarget(self);
    AcquireSRWLockShared(&g_bridgeLock);
    for (UINT i = 0; i < g_targetCount; ++i) {
        if (g_targets[i].target == target) {
            original = g_targets[i].original;
            break;
        }
    }
    ReleaseSRWLockShared(&g_bridgeLock);
    if (!original) return;

    if (type != DXGI_HDR_METADATA_TYPE_HDR10 || size != sizeof(DXGI_HDR_METADATA_HDR10) ||
        !data || !g_hdr10 || !g_hdr10() || !g_peak || IsDisabled()) {
        original(self, processor, streamIndex, type, size, data);
        return;
    }

    const auto* source = static_cast<const DXGI_HDR_METADATA_HDR10*>(data);
    const float peak = g_peak();
    const LONG count = InterlockedIncrement(&g_metadataCalls);
    if (count <= 8 && g_log) {
        g_log(L"HDR10 VP INPUT stream=%u master=%u min=%u CLL=%u FALL=%u displayPeak=%.1f",
            streamIndex, source->MaxMasteringLuminance, source->MinMasteringLuminance,
            source->MaxContentLightLevel, source->MaxFrameAverageLightLevel, peak);
    }

    if (!std::isfinite(peak) || peak < 80.0f || peak > 10000.0f ||
        source->MaxMasteringLuminance == 0 || source->MaxContentLightLevel == 0 ||
        source->MaxMasteringLuminance <= peak ||
        source->MaxContentLightLevel < source->MaxMasteringLuminance) {
        original(self, processor, streamIndex, type, size, data);
        return;
    }

    DXGI_HDR_METADATA_HDR10 adjusted = *source;
    adjusted.MaxMasteringLuminance = static_cast<UINT>(std::clamp(std::lround(peak), 80L, 10000L));
    if (adjusted.MaxMasteringLuminance >= source->MaxMasteringLuminance) {
        original(self, processor, streamIndex, type, size, data);
        return;
    }

    const LONG changes = InterlockedIncrement(&g_metadataAdjustments);
    if ((changes <= 12 || changes % 500 == 0) && g_log) {
        g_log(L"HDR10 VP ADAPT stream=%u originalMaster=%u effectiveMaster=%u originalCLL=%u peak=%.1f mode=DISPLAY_RELATIVE",
            streamIndex, source->MaxMasteringLuminance, adjusted.MaxMasteringLuminance,
            source->MaxContentLightLevel, peak);
    }
    original(self, processor, streamIndex, type, size, &adjusted);
}

static void AttachVideoContext(ID3D11Device* device, ID3D11DeviceContext* immediate) {
    if (!device && !immediate) return;
    bool ownContext = false;
    if (!immediate && device) {
        device->GetImmediateContext(&immediate);
        ownContext = true;
    }
    if (!immediate) return;

    ID3D11VideoContext2* video = nullptr;
    const HRESULT hr = immediate->QueryInterface(IID_ID3D11VideoContext2,
        reinterpret_cast<void**>(&video));
    if (ownContext) immediate->Release();
    if (FAILED(hr) || !video) return;

    void* target = P7Hdr10GetStreamSetTarget(video);
    video->Release();
    if (!target) return;

    AcquireSRWLockExclusive(&g_bridgeLock);
    for (UINT i = 0; i < g_targetCount; ++i) {
        if (g_targets[i].target == target) {
            ReleaseSRWLockExclusive(&g_bridgeLock);
            return;
        }
    }
    if (g_targetCount >= 16) {
        ReleaseSRWLockExclusive(&g_bridgeLock);
        return;
    }
    SetStreamHdrFn original = nullptr;
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void*>(&SetHdrStreamHook),
        reinterpret_cast<void**>(&original));
    if (created == MH_OK && original) {
        const UINT index = g_targetCount++;
        g_targets[index].target = target;
        g_targets[index].original = original;
        const MH_STATUS enabled = MH_EnableHook(target);
        if (g_log) g_log(L"HDR10 VP HOOK target=%p create=%d enable=%d", target, created, enabled);
    } else if (g_log) {
        g_log(L"HDR10 VP HOOK FAILED target=%p status=%d", target, created);
    }
    ReleaseSRWLockExclusive(&g_bridgeLock);
}

static HRESULT WINAPI CreateDeviceHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk,
    ID3D11Device** device, D3D_FEATURE_LEVEL* feature, ID3D11DeviceContext** context) {
    const HRESULT hr = g_createDevice(adapter, type, software, flags, levels, levelCount,
        sdk, device, feature, context);
    if (SUCCEEDED(hr)) AttachVideoContext(device ? *device : nullptr, context ? *context : nullptr);
    return hr;
}

static HRESULT WINAPI CreateDeviceAndSwapChainHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* feature, ID3D11DeviceContext** context) {
    const HRESULT hr = g_createDeviceAndSwapChain(adapter, type, software, flags, levels,
        levelCount, sdk, desc, swapChain, device, feature, context);
    if (SUCCEEDED(hr)) AttachVideoContext(device ? *device : nullptr, context ? *context : nullptr);
    return hr;
}

extern "C" void InstallHdr10VideoProcessorBridge(Hdr10ActiveFn active,
    DisplayPeakFn peak, LogFn logger) {
    g_hdr10 = active;
    g_peak = peak;
    g_log = logger;
    HMODULE dll = GetModuleHandleW(L"d3d11.dll");
    if (!dll) dll = LoadLibraryW(L"d3d11.dll");
    if (!dll) return;

    void* create = reinterpret_cast<void*>(GetProcAddress(dll, "D3D11CreateDevice"));
    void* combined = reinterpret_cast<void*>(GetProcAddress(dll, "D3D11CreateDeviceAndSwapChain"));
    if (create) {
        const MH_STATUS result = MH_CreateHook(create, reinterpret_cast<void*>(&CreateDeviceHook),
            reinterpret_cast<void**>(&g_createDevice));
        if (result == MH_OK) MH_EnableHook(create);
        if (g_log) g_log(L"HDR10 VP D3D11CreateDevice hook=%d", result);
    }
    if (combined) {
        const MH_STATUS result = MH_CreateHook(combined,
            reinterpret_cast<void*>(&CreateDeviceAndSwapChainHook),
            reinterpret_cast<void**>(&g_createDeviceAndSwapChain));
        if (result == MH_OK) MH_EnableHook(combined);
        if (g_log) g_log(L"HDR10 VP D3D11CreateDeviceAndSwapChain hook=%d", result);
    }
}

extern "C" void AttachHdr10VideoDevice(void* device) {
    AttachVideoContext(static_cast<ID3D11Device*>(device), nullptr);
}
