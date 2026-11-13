#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>
#include "Hdr10ToneMapMath.h"

using Microsoft::WRL::ComPtr;

int wmain() {
    ComPtr<IDXGIFactory1> factory;
    const HRESULT factoryHr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factoryHr)) {
        std::wprintf(L"DXGI factory error: 0x%08X\n", static_cast<unsigned>(factoryHr));
        return 2;
    }

    unsigned displays = 0;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapterHr = factory->EnumAdapters1(a, &adapter);
        if (adapterHr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(adapterHr)) continue;

        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (FAILED(adapter->GetDesc1(&adapterDesc)) ||
            (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;

        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            const HRESULT outputHr = adapter->EnumOutputs(o, &output);
            if (outputHr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(outputHr)) continue;

            DXGI_OUTPUT_DESC outputDesc{};
            if (FAILED(output->GetDesc(&outputDesc))) continue;
            if (!outputDesc.AttachedToDesktop) continue;
            ++displays;
            ComPtr<IDXGIOutput6> output6;
            const HRESULT queryHr = output.As(&output6);
            if (FAILED(queryHr)) {
                std::wprintf(L"%ls: HDR information unavailable (IDXGIOutput6, 0x%08X)\n",
                             outputDesc.DeviceName, static_cast<unsigned>(queryHr));
                continue;
            }
            DXGI_OUTPUT_DESC1 desc{};
            const HRESULT hr = output6->GetDesc1(&desc);
            if (FAILED(hr)) {
                std::wprintf(L"%ls: GetDesc1 failed (0x%08X)\n",
                             outputDesc.DeviceName, static_cast<unsigned>(hr));
                continue;
            }
            const bool hdrActive = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            std::wprintf(L"%ls | active HDR mode: %ls | DXGI color space: %u\n",
                         desc.DeviceName, hdrActive ? L"YES (PQ/BT.2020)" : L"NO or non-PQ",
                         static_cast<unsigned>(desc.ColorSpace));
            std::wprintf(L"  luminance min=%.4f max=%.1f max-full-frame=%.1f nits\n",
                         desc.MinLuminance, desc.MaxLuminance, desc.MaxFullFrameLuminance);
            const Hdr10ToneMap::Plan example = Hdr10ToneMap::MakePlan(
                {4000.0, 6248.0, 902.0},
                {static_cast<double>(desc.MaxLuminance), static_cast<double>(desc.MaxFullFrameLuminance)});
            if (example.valid && hdrActive) {
                std::wprintf(L"  HDR10 4000/6248 reference curve: %ls, black=%.4f, 1000 nits -> %.1f nits, max -> %.1f nits\n",
                    example.compression ? L"COMPRESSED" : L"UNCHANGED",
                    Hdr10ToneMap::MapNits(0.0, example), Hdr10ToneMap::MapNits(1000.0, example),
                    Hdr10ToneMap::MapNits(example.sourcePeak, example));
            } else {
                std::wprintf(L"  HDR10 tone map reference unavailable (HDR mode off or invalid display peak)\n");
            }
        }
    }
    if (displays == 0) {
        std::wprintf(L"No desktop-attached DXGI output found.\n");
        return 1;
    }
    std::wprintf(L"This checks the Windows display mode, not the player's swap chain.\n");
    return 0;
}
