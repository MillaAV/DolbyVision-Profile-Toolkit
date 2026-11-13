#pragma once

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cwchar>

struct Hdr10DisplayQueryResult {
    bool available = false;
    bool hdr10Mode = false;
    wchar_t deviceName[32]{};
    float peakNits = 0.0f;
    float minNits = 0.0f;
    float fullFrameNits = 0.0f;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
};

struct Hdr10WindowSearch {
    DWORD processId = 0;
    HWND found = nullptr;
};

static BOOL CALLBACK FindHdr10PlayerWindow(HWND hwnd, LPARAM parameter) {
    auto* result = reinterpret_cast<Hdr10WindowSearch*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != result->processId || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) return TRUE;
    result->found = hwnd;
    return FALSE;
}

static Hdr10DisplayQueryResult QueryHdr10PlayerDisplay() {
    Hdr10DisplayQueryResult result{};
    Hdr10WindowSearch search{ GetCurrentProcessId(), nullptr };
    EnumWindows(FindHdr10PlayerWindow, reinterpret_cast<LPARAM>(&search));
    HMONITOR windowMonitor = search.found
        ? MonitorFromWindow(search.found, MONITOR_DEFAULTTONULL) : nullptr;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;

    Hdr10DisplayQueryResult fallback{};
    UINT fallbackCount = 0;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;
        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (!output) continue;
            DXGI_OUTPUT_DESC outputDesc{};
            if (FAILED(output->GetDesc(&outputDesc)) || !outputDesc.AttachedToDesktop)
                continue;

            Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6))) continue;
            DXGI_OUTPUT_DESC1 desc{};
            if (FAILED(output6->GetDesc1(&desc))) continue;

            Hdr10DisplayQueryResult candidate{};
            candidate.available = true;
            candidate.hdr10Mode = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            candidate.peakNits = desc.MaxLuminance;
            candidate.minNits = desc.MinLuminance;
            candidate.fullFrameNits = desc.MaxFullFrameLuminance;
            candidate.colorSpace = desc.ColorSpace;
            wcsncpy_s(candidate.deviceName, desc.DeviceName, _TRUNCATE);

            if (windowMonitor && outputDesc.Monitor == windowMonitor) return candidate;
            if (candidate.hdr10Mode && candidate.peakNits >= 80.0f &&
                candidate.peakNits <= 10000.0f) {
                fallback = candidate;
                ++fallbackCount;
            }
        }
    }
    if (fallbackCount == 1) return fallback;
    return result;
}
