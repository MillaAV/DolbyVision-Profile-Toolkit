#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <propvarutil.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <deque>
#include <cstdint>
#include <algorithm>
#include <cwchar>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <limits>
#include <array>
#include <thread>
#include <unordered_set>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "MinHook.h"

using Microsoft::WRL::ComPtr;

using MFTEnumExFn = HRESULT (WINAPI*)(
    GUID,
    UINT32,
    const MFT_REGISTER_TYPE_INFO*,
    const MFT_REGISTER_TYPE_INFO*,
    IMFActivate***,
    UINT32*);

using SetInputTypeFn = HRESULT (STDMETHODCALLTYPE*)(IMFTransform*, DWORD, IMFMediaType*, DWORD);
using SetOutputTypeFn = HRESULT (STDMETHODCALLTYPE*)(IMFTransform*, DWORD, IMFMediaType*, DWORD);
using ProcessInputFn = HRESULT (STDMETHODCALLTYPE*)(IMFTransform*, DWORD, IMFSample*, DWORD);
using ProcessOutputFn = HRESULT (STDMETHODCALLTYPE*)(IMFTransform*, DWORD, DWORD, MFT_OUTPUT_DATA_BUFFER*, DWORD*);
using ProcessMessageFn = HRESULT (STDMETHODCALLTYPE*)(IMFTransform*, MFT_MESSAGE_TYPE, ULONG_PTR);

static MFTEnumExFn g_originalMFTEnumEx = nullptr;
static SetInputTypeFn g_originalHevcSetInputType = nullptr;
static SetOutputTypeFn g_originalHevcSetOutputType = nullptr;
static ProcessInputFn g_originalHevcProcessInput = nullptr;
static ProcessOutputFn g_originalHevcProcessOutput = nullptr;
static ProcessMessageFn g_originalHevcProcessMessage = nullptr;
static ProcessInputFn g_originalDolbyProcessInput = nullptr;
static ProcessOutputFn g_originalDolbyProcessOutput = nullptr;
static SRWLOCK g_logLock = SRWLOCK_INIT;
static SRWLOCK g_felLock = SRWLOCK_INIT;
static volatile LONG g_hookCalls = 0;
static volatile LONG g_decoderHookState = 0; // 0=not tried, 1=installing, 2=installed
static volatile LONG g_felContentActive = 0;
static SRWLOCK g_p7CandidateLock = SRWLOCK_INIT;
static std::unordered_set<IMFTransform*> g_p7CandidateDecoders;
static volatile LONG g_felCaptureCount = 0;
static volatile LONG g_felBridgeCount = 0;
static volatile LONG g_felBridgeMissCount = 0;
static volatile LONG g_dolbyHookState = 0;
static volatile LONG g_dolbyInputFramesLogged = 0;
static volatile LONG g_dolbyOutputFramesLogged = 0;
static volatile LONG g_dolbyOutputCalls = 0;
static volatile LONG g_dolbyP016Available = 0;
static volatile LONG g_dolbyP016Frames = 0;
static volatile LONG g_dolbyP016Fallbacks = 0;
static volatile LONG g_dolbyP016Switches = 0;
static volatile LONG g_elDecoderInitState = 0;
static volatile LONG g_elDecodedFrames = 0;
static volatile LONG g_elDecodeInputs = 0;
static volatile LONG g_rpuParsedFrames = 0;
static volatile LONG g_rpuFelFrames = 0;
static volatile LONG g_rpuNlqFrames = 0;
static volatile LONG g_felTripletFrames = 0;
static volatile LONG g_blDxgiFrames = 0;
static volatile LONG g_tripletProbeCount = 0;
static volatile LONG g_felReconAttempts = 0;
static volatile LONG g_felReconApplied = 0;
static volatile LONG g_felReconMisses = 0;
static volatile LONG g_felReconFailures = 0;
static volatile LONG g_gpuRendererReady = 0;
static volatile LONG g_gpuQueuedFrames = 0;
static volatile LONG g_gpuAppliedFrames = 0;
static volatile LONG g_gpuMissFrames = 0;
static SRWLOCK g_gpuLock = SRWLOCK_INIT;
static SRWLOCK g_gpuQueueLock = SRWLOCK_INIT;
static IMFTransform* g_elDecoder = nullptr;
static GUID g_elOutputSubtype = GUID_NULL;
static UINT32 g_elOutputWidth = 0;
static UINT32 g_elOutputHeight = 0;
static SRWLOCK g_elDecoderLock = SRWLOCK_INIT;
static bool g_felBridgeEnabled = true;
static bool g_low6CarrierEnabled = true;
static volatile LONG g_felSplitEnabled = 0;
static volatile LONGLONG g_lastSplitModeRefresh = 0;
static bool g_felHighlightLimitEnabled = false;
static volatile LONG g_dolbyLow6OutputProofDone = 0;
static HMODULE g_selfModule = nullptr;
static HMODULE g_doviModule = nullptr;
static SRWLOCK g_doviLock = SRWLOCK_INIT;

static void Log(const wchar_t* fmt, ...);
static bool ReadFriendlyName(IMFActivate* activate, wchar_t* name, UINT32 cch);

static const GUID kForwardCustomNalu =
{ 0xed336efd, 0x244f, 0x428d, { 0x91, 0x53, 0x28, 0xf3, 0x99, 0x45, 0x88, 0x90 } };

static const GUID kHevcEsSubtype =
{ 0x53564548, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static const GUID kForwardCustomSeiDecodeOrder =
{ 0xf13bbe3c, 0x36d4, 0x410a, { 0xb9, 0x85, 0x7a, 0x95, 0x1a, 0x1e, 0x62, 0x94 } };


static const GUID kForwardedDecodeUnits =
{ 0x424c754c, 0x97c8, 0x48d6, { 0x87, 0x77, 0xfc, 0x41, 0xf7, 0xb6, 0x08, 0x79 } };

static const GUID kForwardedDecodeUnitType =
{ 0x089e57c7, 0x47d3, 0x4a26, { 0xbf, 0x9c, 0x4b, 0x64, 0xfa, 0xfb, 0x5d, 0x1e } };

static constexpr UINT32 kDecodeUnitNal = 0;
static constexpr UINT32 kDolbyVisionFelNaluType = 63;

static constexpr UINT32 kDolbyVisionHevcNaluType = 62;

static const GUID kRendererExtensionProfile =
{ 0x8437d4b9, 0xd448, 0x4fcd, { 0x9b, 0x6b, 0x83, 0x9b, 0xf9, 0x6c, 0x77, 0x98 } };

static const GUID kMatroskaCodecId =
{ 0x9668dde5, 0xc869, 0x4d6a, { 0xb0, 0x2e, 0x46, 0x3b, 0x7c, 0x23, 0x1b, 0x53 } };


static bool ParseFelModeFile(const wchar_t* modePath, bool* found) {
    if (found) *found = false;
    if (!modePath || !*modePath) return true;

    HANDLE h = CreateFileW(
        modePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        return true;
    }

    if (found) *found = true;

    char buf[64]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(h);

    if (!ok || read == 0) {
        return true;
    }

    for (DWORD i = 0; i < read; ++i) {
        if (buf[i] >= 'a' && buf[i] <= 'z') {
            buf[i] = static_cast<char>(buf[i] - ('a' - 'A'));
        }
    }

    if (buf[0] == '0') return false;
    if (read >= 3 && buf[0] == 'O' && buf[1] == 'F' && buf[2] == 'F') return false;
    if (read >= 2 && buf[0] == 'N' && buf[1] == 'O') return false;

    return true;
}

static bool ReadFelBridgeEnabled(wchar_t* usedPath, size_t usedPathChars) {
    if (usedPath && usedPathChars) usedPath[0] = L'\0';

    wchar_t localMode[32768]{};

    if (g_selfModule &&
        GetModuleFileNameW(g_selfModule, localMode, _countof(localMode))) {

        wchar_t* slash = wcsrchr(localMode, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscat_s(localMode, L"fel-bridge-mode.txt");

            bool found = false;
            const bool enabled = ParseFelModeFile(localMode, &found);

            if (found) {
                if (usedPath && usedPathChars) {
                    wcsncpy_s(usedPath, usedPathChars, localMode, _TRUNCATE);
                }
                return enabled;
            }
        }
    }

    const wchar_t* central =
        L"C:\\ProgramData\\MillaAV\\P7DV\\fel-bridge-mode.txt";

    bool centralFound = false;
    const bool enabled = ParseFelModeFile(central, &centralFound);

    if (usedPath && usedPathChars) {
        if (centralFound) {
            wcsncpy_s(usedPath, usedPathChars, central, _TRUNCATE);
        } else {
            wcscpy_s(usedPath, usedPathChars, L"<default-ON:no-readable-mode-file>");
        }
    }

    return enabled;
}

static bool ReadLow6CarrierEnabled(wchar_t* usedPath, size_t usedPathChars) {
    if (usedPath && usedPathChars) usedPath[0] = L'\0';

    wchar_t localMode[32768]{};
    if (g_selfModule && GetModuleFileNameW(g_selfModule, localMode, _countof(localMode))) {
        wchar_t* slash = wcsrchr(localMode, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscat_s(localMode, L"fel-low6-mode.txt");
            bool found = false;
            const bool enabled = ParseFelModeFile(localMode, &found);
            if (found) {
                if (usedPath && usedPathChars)
                    wcsncpy_s(usedPath, usedPathChars, localMode, _TRUNCATE);
                return enabled;
            }
        }
    }

    const wchar_t* central = L"C:\\ProgramData\\MillaAV\\P7DV\\fel-low6-mode.txt";
    bool found = false;
    const bool enabled = ParseFelModeFile(central, &found);
    if (usedPath && usedPathChars) {
        if (found) wcsncpy_s(usedPath, usedPathChars, central, _TRUNCATE);
        else wcscpy_s(usedPath, usedPathChars, L"<default-ON:no-readable-low6-mode-file>");
    }
    return enabled;
}

static LONG ParseFelSplitModeFile(const wchar_t* modePath, bool* found) {
    if (found) *found = false;
    bool enabled = ParseFelModeFile(modePath, found);
    if (!enabled || !found || !*found) return 0;
    HANDLE h = CreateFileW(modePath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    char buf[32]{};
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);
    if (!ok || n == 0) return 1;
    for (DWORD i = 0; i < n; ++i)
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= ('a' - 'A');
    if (n >= 4 && buf[0] == 'D' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F') return 2;
    return 1;
}

static LONG ReadFelSplitEnabled(wchar_t* usedPath, size_t usedPathChars) {
    if (usedPath && usedPathChars) usedPath[0] = L'\0';
    wchar_t localMode[32768]{};
    if (g_selfModule && GetModuleFileNameW(g_selfModule, localMode, _countof(localMode))) {
        wchar_t* slash = wcsrchr(localMode, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscat_s(localMode, L"fel-split-mode.txt");
            bool found = false;
            const LONG mode = ParseFelSplitModeFile(localMode, &found);
            if (found) {
                if (usedPath && usedPathChars)
                    wcsncpy_s(usedPath, usedPathChars, localMode, _TRUNCATE);
                return mode;
            }
        }
    }

    const wchar_t* central = L"C:\\ProgramData\\MillaAV\\P7DV\\fel-split-mode.txt";
    bool found = false;
    const LONG mode = ParseFelSplitModeFile(central, &found);
    if (usedPath && usedPathChars) {
        if (found) wcsncpy_s(usedPath, usedPathChars, central, _TRUNCATE);
        else wcscpy_s(usedPath, usedPathChars, L"<default-OFF>");
    }
    return found ? mode : 0;
}

static void RefreshFelSplitMode() {
    const LONGLONG now = static_cast<LONGLONG>(GetTickCount64());
    const LONGLONG previous = InterlockedCompareExchange64(&g_lastSplitModeRefresh, 0, 0);
    if (now - previous < 800) return;
    if (InterlockedCompareExchange64(&g_lastSplitModeRefresh, now, previous) != previous) return;

    wchar_t path[32768]{};
    const LONG enabled = ReadFelSplitEnabled(path, _countof(path));
    const LONG before = InterlockedExchange(&g_felSplitEnabled, enabled);
    if (before != enabled) {
        Log(L"FEL SPLIT MODE CHANGED=%s modeFile=%s", enabled == 2 ? L"DIFF" : (enabled ? L"ON" : L"OFF"), path);
    }
}

static bool ReadFelLumaGuardEnabled(wchar_t* usedPath, size_t usedPathChars) {
    if (usedPath && usedPathChars) usedPath[0] = L'\0';
    wchar_t localMode[32768]{};
    if (g_selfModule && GetModuleFileNameW(g_selfModule, localMode, _countof(localMode))) {
        wchar_t* slash = wcsrchr(localMode, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscat_s(localMode, L"fel-luma-guard-mode.txt");
            bool found = false;
            const bool enabled = ParseFelModeFile(localMode, &found);
            if (found) {
                if (usedPath && usedPathChars)
                    wcsncpy_s(usedPath, usedPathChars, localMode, _TRUNCATE);
                return enabled;
            }
        }
    }

    const wchar_t* central = L"C:\\ProgramData\\MillaAV\\P7DV\\fel-luma-guard-mode.txt";
    bool found = false;
    const bool enabled = ParseFelModeFile(central, &found);
    if (usedPath && usedPathChars) {
        if (found) wcsncpy_s(usedPath, usedPathChars, central, _TRUNCATE);
        else wcscpy_s(usedPath, usedPathChars, L"<default-OFF>");
    }
    return found ? enabled : false;
}

static void HashUpdate64(uint64_t& h, const BYTE* data, size_t size) {
    if (!data || size == 0) return;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;
    }
}

static uint64_t HashDolbyOutputPixels(
    IMFMediaBuffer* buffer,
    IMFMediaType* type,
    DWORD* readableBytes,
    HRESULT* outHr) {

    if (readableBytes) *readableBytes = 0;
    if (outHr) *outHr = E_FAIL;
    if (!buffer) return 0;

    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;

    if (type) {
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
    }

    IMF2DBuffer2* twoD = nullptr;
    HRESULT hr = buffer->QueryInterface(IID_PPV_ARGS(&twoD));

    if (SUCCEEDED(hr) && twoD) {
        BYTE* scan0 = nullptr;
        BYTE* bufferStart = nullptr;
        LONG pitch = 0;
        DWORD bufferLength = 0;

        hr = twoD->Lock2DSize(
            MF2DBuffer_LockFlags_Read,
            &scan0,
            &pitch,
            &bufferStart,
            &bufferLength);

        if (SUCCEEDED(hr) && scan0) {
            uint64_t h = 1469598103934665603ULL;
            DWORD bytesHashed = 0;

            const bool p010 = subtype == MFVideoFormat_P010;
            const bool nv12 = subtype == MFVideoFormat_NV12;
            const UINT32 rowBytes = p010 ? width * 2u : (nv12 ? width : 0u);

            const size_t absPitch = pitch > 0 ? static_cast<size_t>(pitch) : 0;
            const size_t totalRows =
                static_cast<size_t>(height) + static_cast<size_t>(height / 2u);
            const size_t span = totalRows
                ? (absPitch * (totalRows - 1u) + rowBytes)
                : 0;

            const BYTE* bufferEnd =
                bufferStart ? (bufferStart + bufferLength) : nullptr;

            const bool activeRangeSafe =
                bufferStart &&
                bufferEnd &&
                scan0 >= bufferStart &&
                span > 0 &&
                scan0 + span <= bufferEnd;

            if ((p010 || nv12) &&
                width &&
                height &&
                pitch > 0 &&
                static_cast<UINT32>(pitch) >= rowBytes &&
                activeRangeSafe) {

                for (UINT32 y = 0; y < height; ++y) {
                    const BYTE* row =
                        scan0 + static_cast<size_t>(pitch) * y;
                    HashUpdate64(h, row, rowBytes);
                    bytesHashed += rowBytes;
                }

                const BYTE* chroma =
                    scan0 + static_cast<size_t>(pitch) * height;

                for (UINT32 y = 0; y < height / 2u; ++y) {
                    const BYTE* row =
                        chroma + static_cast<size_t>(pitch) * y;
                    HashUpdate64(h, row, rowBytes);
                    bytesHashed += rowBytes;
                }
            }
            else if (bufferStart && bufferLength) {
                HashUpdate64(h, bufferStart, bufferLength);
                bytesHashed = bufferLength;
            }

            twoD->Unlock2D();
            twoD->Release();

            if (readableBytes) *readableBytes = bytesHashed;
            if (outHr) *outHr = S_OK;
            return h;
        }

        twoD->Release();
    }

    BYTE* data = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;

    hr = buffer->Lock(&data, &maxLen, &curLen);
    if (SUCCEEDED(hr) && data) {
        uint64_t h = 1469598103934665603ULL;
        HashUpdate64(h, data, curLen);
        buffer->Unlock();

        if (readableBytes) *readableBytes = curLen;
        if (outHr) *outHr = S_OK;
        return h;
    }

    if (outHr) *outHr = hr;
    return 0;
}

static void LogDolbyOutputHash(
    IMFTransform* self,
    IMFSample* sample,
    DWORD streamId,
    LONG frameIndex) {

    if (!self || !sample) return;

    LONGLONG sampleTime = -1;
    sample->GetSampleTime(&sampleTime);

    IMFMediaType* outputType = nullptr;
    self->GetOutputCurrentType(streamId, &outputType);

    IMFMediaBuffer* buffer = nullptr;
    HRESULT bufferHr = sample->GetBufferByIndex(0, &buffer);

    if (FAILED(bufferHr) || !buffer) {
        Log(L"FEL DOLBY_OUT_HASH frame=%ld time=%lld hash=0000000000000000 bytes=0 hr=0x%08X",
            frameIndex,
            static_cast<long long>(sampleTime),
            static_cast<unsigned>(bufferHr));

        if (outputType) outputType->Release();
        return;
    }

    DWORD bytes = 0;
    HRESULT hashHr = E_FAIL;
    const uint64_t hash =
        HashDolbyOutputPixels(buffer, outputType, &bytes, &hashHr);

    Log(L"FEL DOLBY_OUT_HASH frame=%ld time=%lld hash=%016llX bytes=%lu hr=0x%08X mode=%s",
        frameIndex,
        static_cast<long long>(sampleTime),
        static_cast<unsigned long long>(hash),
        bytes,
        static_cast<unsigned>(hashHr),
        g_felBridgeEnabled ? L"ON" : L"OFF");

    buffer->Release();
    if (outputType) outputType->Release();
}




struct DoviU16DataC {
    const uint16_t* data;
    size_t len;
};

struct DoviU64DataC {
    const uint64_t* data;
    size_t len;
};

struct DoviI64DataC {
    const int64_t* data;
    size_t len;
};

struct DoviU64Data2DC {
    const DoviU64DataC* const* list;
    size_t len;
};

struct DoviI64Data2DC {
    const DoviI64DataC* const* list;
    size_t len;
};

struct DoviU64Data3DC {
    const DoviU64Data2DC* const* list;
    size_t len;
};

struct DoviI64Data3DC {
    const DoviI64Data2DC* const* list;
    size_t len;
};

struct DoviPolynomialCurveC {
    DoviU64DataC polyOrderMinus1;
    struct { const uint8_t* data; size_t len; } linearInterpFlag;
    DoviI64Data2DC polyCoefInt;
    DoviU64Data2DC polyCoef;
};

struct DoviMmrCurveC {
    struct { const uint8_t* data; size_t len; } mmrOrderMinus1;
    DoviI64DataC mmrConstantInt;
    DoviU64DataC mmrConstant;
    DoviI64Data3DC mmrCoefInt;
    DoviU64Data3DC mmrCoef;
};

struct DoviReshapingCurveC {
    uint64_t numPivotsMinus2;
    DoviU16DataC pivots;
    uint8_t mappingIdc;
    const DoviPolynomialCurveC* polynomial;
    const DoviMmrCurveC* mmr;
};

struct DoviRpuDataNlqC {
    uint16_t nlqOffset[3];
    uint64_t vdrInMaxInt[3];
    uint64_t vdrInMax[3];
    uint64_t linearDeadzoneSlopeInt[3];
    uint64_t linearDeadzoneSlope[3];
    uint64_t linearDeadzoneThresholdInt[3];
    uint64_t linearDeadzoneThreshold[3];
};

struct DoviRpuDataMappingC {
    uint64_t vdrRpuId;
    uint64_t mappingColorSpace;
    uint64_t mappingChromaFormatIdc;
    uint64_t numXPartitionsMinus1;
    uint64_t numYPartitionsMinus1;
    DoviReshapingCurveC curves[3];
    int32_t nlqMethodIdc;
    int32_t nlqNumPivotsMinus2;
    DoviU16DataC nlqPredPivotValue;
    const DoviRpuDataNlqC* nlq;
};

struct DoviRpuDataHeaderC {
    uint8_t guessedProfile;
    const char* elType;
    uint8_t rpuNalPrefix;
    uint8_t rpuType;
    uint16_t rpuFormat;
    uint8_t vdrRpuProfile;
    uint8_t vdrRpuLevel;
    bool vdrSeqInfoPresentFlag;
    bool chromaResamplingExplicitFilterFlag;
    uint8_t coefficientDataType;
    uint64_t coefficientLog2Denom;
    uint8_t vdrRpuNormalizedIdc;
    bool blVideoFullRangeFlag;
    uint64_t blBitDepthMinus8;
    uint64_t elBitDepthMinus8;
    uint64_t vdrBitDepthMinus8;
    bool spatialResamplingFilterFlag;
    uint8_t reservedZero3bits;
    bool elSpatialResamplingFilterFlag;
    bool disableResidualFlag;
    bool vdrDmMetadataPresentFlag;
    bool usePrevVdrRpuFlag;
    uint64_t prevVdrRpuId;
};

struct DoviVdrDmDataPrefixC {
    bool compressed;
    uint64_t affectedDmMetadataId;
    uint64_t currentDmMetadataId;
    uint64_t sceneRefreshFlag;
    int16_t yccToRgbCoef[9];
    uint32_t yccToRgbOffset[3];
    int16_t rgbToLmsCoef[9];
    uint16_t signalEotf;
    uint16_t signalEotfParam0;
    uint16_t signalEotfParam1;
    uint32_t signalEotfParam2;
    uint8_t signalBitDepth;
    uint8_t signalColorSpace;
    uint8_t signalChromaFormat;
    uint8_t signalFullRangeFlag;
    uint16_t sourceMinPq;
    uint16_t sourceMaxPq;
    uint16_t sourceDiagonal;
};

using DoviParseUnspec62Fn = void* (__cdecl*)(const uint8_t*, size_t);
using DoviRpuFreeFn = void (__cdecl*)(void*);
using DoviGetHeaderFn = const DoviRpuDataHeaderC* (__cdecl*)(const void*);
using DoviFreeHeaderFn = void (__cdecl*)(const DoviRpuDataHeaderC*);
using DoviGetMappingFn = const DoviRpuDataMappingC* (__cdecl*)(const void*);
using DoviFreeMappingFn = void (__cdecl*)(const DoviRpuDataMappingC*);
using DoviGetVdrDmFn = const DoviVdrDmDataPrefixC* (__cdecl*)(const void*);
using DoviFreeVdrDmFn = void (__cdecl*)(const DoviVdrDmDataPrefixC*);
using DoviGetErrorFn = const char* (__cdecl*)(const void*);

static DoviParseUnspec62Fn g_doviParseUnspec62 = nullptr;
static DoviRpuFreeFn g_doviRpuFree = nullptr;
static DoviGetHeaderFn g_doviGetHeader = nullptr;
static DoviFreeHeaderFn g_doviFreeHeader = nullptr;
static DoviGetMappingFn g_doviGetMapping = nullptr;
static DoviFreeMappingFn g_doviFreeMapping = nullptr;
static DoviGetVdrDmFn g_doviGetVdrDm = nullptr;
static DoviFreeVdrDmFn g_doviFreeVdrDm = nullptr;
static DoviGetErrorFn g_doviGetError = nullptr;

struct RpuCurveInfo {
    uint32_t numPivots = 0;
    uint32_t mappingIdc = 0xFFFFFFFFu;
    float pivots[9]{};
    float polyCoeff[8][3]{};
    uint32_t polyOrder[8]{};
    float mmrConstant[8]{};
    uint32_t mmrOrder[8]{};
    float mmrCoeff[8][3][7]{};
};

struct DolbyColorInfo {
    bool valid = false;
    float nonlinear[3][3]{};
    float nonlinearOffset[3]{};
    float linear[3][3]{};
};

static SRWLOCK g_doviColorStateLock = SRWLOCK_INIT;
static DolbyColorInfo g_lastDoviColor{};

struct ParsedRpuInfo {
    bool parsed = false;
    uint8_t guessedProfile = 0;
    bool profile7 = false;
    bool fel = false;
    bool mel = false;
    bool nlqActive = false;
    bool renderMetadataReady = false;
    uint8_t coefficientDataType = 0;
    uint64_t coefficientLog2Denom = 0;
    uint64_t blBitDepth = 0;
    uint64_t elBitDepth = 0;
    bool blVideoFullRange = false;
    int32_t nlqMethodIdc = -1;
    uint16_t offset[3]{};
    uint64_t slopeInt[3]{};
    uint64_t slope[3]{};
    uint64_t thresholdInt[3]{};
    uint64_t threshold[3]{};
    float nlqOffsetNorm[3]{};
    float nlqSlopeNorm[3]{};
    float nlqThresholdNorm[3]{};
    RpuCurveInfo curves[3];
    DolbyColorInfo color;
};

static bool LoadDoviApi() {
    AcquireSRWLockExclusive(&g_doviLock);
    if (g_doviModule && g_doviParseUnspec62 && g_doviGetHeader && g_doviGetMapping) {
        ReleaseSRWLockExclusive(&g_doviLock);
        return true;
    }

    wchar_t modulePath[32768]{};
    wchar_t dllPath[32768]{};
    HMODULE mod = nullptr;

    if (g_selfModule && GetModuleFileNameW(g_selfModule, modulePath, _countof(modulePath))) {
        wchar_t* slash = wcsrchr(modulePath, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscpy_s(dllPath, modulePath);
            wcscat_s(dllPath, L"dovi.dll");
            mod = LoadLibraryW(dllPath);
            if (!mod) {
                wcscpy_s(dllPath, modulePath);
                wcscat_s(dllPath, L"libdovi.dll");
                mod = LoadLibraryW(dllPath);
            }
        }
    }

    if (!mod) mod = LoadLibraryW(L"C:\\ProgramData\\MillaAV\\P7DV\\dovi.dll");
    if (!mod) mod = LoadLibraryW(L"C:\\ProgramData\\MillaAV\\P7DV\\libdovi.dll");

    if (!mod) {
        Log(L"FEL LIBDOVI load FAILED winerr=%lu", GetLastError());
        ReleaseSRWLockExclusive(&g_doviLock);
        return false;
    }

    g_doviParseUnspec62 = reinterpret_cast<DoviParseUnspec62Fn>(GetProcAddress(mod, "dovi_parse_unspec62_nalu"));
    g_doviRpuFree = reinterpret_cast<DoviRpuFreeFn>(GetProcAddress(mod, "dovi_rpu_free"));
    g_doviGetHeader = reinterpret_cast<DoviGetHeaderFn>(GetProcAddress(mod, "dovi_rpu_get_header"));
    g_doviFreeHeader = reinterpret_cast<DoviFreeHeaderFn>(GetProcAddress(mod, "dovi_rpu_free_header"));
    g_doviGetMapping = reinterpret_cast<DoviGetMappingFn>(GetProcAddress(mod, "dovi_rpu_get_data_mapping"));
    g_doviFreeMapping = reinterpret_cast<DoviFreeMappingFn>(GetProcAddress(mod, "dovi_rpu_free_data_mapping"));
    g_doviGetVdrDm = reinterpret_cast<DoviGetVdrDmFn>(GetProcAddress(mod, "dovi_rpu_get_vdr_dm_data"));
    g_doviFreeVdrDm = reinterpret_cast<DoviFreeVdrDmFn>(GetProcAddress(mod, "dovi_rpu_free_vdr_dm_data"));
    g_doviGetError = reinterpret_cast<DoviGetErrorFn>(GetProcAddress(mod, "dovi_rpu_get_error"));

    if (!g_doviParseUnspec62 || !g_doviRpuFree || !g_doviGetHeader || !g_doviFreeHeader ||
        !g_doviGetMapping || !g_doviFreeMapping || !g_doviGetVdrDm || !g_doviFreeVdrDm) {
        Log(L"FEL LIBDOVI exports missing");
        FreeLibrary(mod);
        g_doviModule = nullptr;
        ReleaseSRWLockExclusive(&g_doviLock);
        return false;
    }

    g_doviModule = mod;
    Log(L"FEL LIBDOVI READY module=%p", mod);
    ReleaseSRWLockExclusive(&g_doviLock);
    return true;
}

struct CapturedDvUnit {
    UINT32 nalType = 0;
    std::vector<BYTE> bytes; // ForwardedDecodeUnits representation: EPB removed
    std::vector<BYTE> rawBytes; // Original NAL bytes: header + EBSP, no start code/length
};

struct CapturedDvAccessUnit {
    LONGLONG sampleTime = -1;
    std::vector<CapturedDvUnit> units; // bitstream order, only NAL 62/63
    ParsedRpuInfo rpu;
};

static SRWLOCK g_dolbyFelQueueLock = SRWLOCK_INIT;
static std::deque<CapturedDvAccessUnit> g_dolbyFelQueue;
static CapturedDvAccessUnit g_lastDolbyFelAu;
static bool g_haveLastDolbyFelAu = false;
static LONG g_dolbyFelReplayCount = 0;


static void QueueDolbyFelAu(const CapturedDvAccessUnit& au) {
    AcquireSRWLockExclusive(&g_dolbyFelQueueLock);
    g_dolbyFelQueue.push_back(au);
    while (g_dolbyFelQueue.size() > 32) g_dolbyFelQueue.pop_front();
    ReleaseSRWLockExclusive(&g_dolbyFelQueueLock);
}

static bool TakeDolbyFelAu(LONGLONG sampleTime, CapturedDvAccessUnit& out) {
    bool found = false;
    AcquireSRWLockExclusive(&g_dolbyFelQueueLock);
    for (auto it = g_dolbyFelQueue.begin(); it != g_dolbyFelQueue.end(); ++it) {
        if (it->sampleTime == sampleTime) {
            out = *it;
            g_lastDolbyFelAu = out;
            g_haveLastDolbyFelAu = true;
            g_dolbyFelQueue.erase(it);
            found = true;
            break;
        }
    }
    if (!found && g_haveLastDolbyFelAu &&
        g_lastDolbyFelAu.sampleTime == sampleTime) {
        out = g_lastDolbyFelAu;
        found = true;
        if (InterlockedIncrement(&g_dolbyFelReplayCount) <= 32)
            Log(L"FEL DOLBY AU REPLAY same PTS=%lld", static_cast<long long>(sampleTime));
    }
    while (g_dolbyFelQueue.size() > 24) g_dolbyFelQueue.pop_front();
    ReleaseSRWLockExclusive(&g_dolbyFelQueueLock);
    return found;
}

static std::deque<CapturedDvAccessUnit> g_dvQueue;

static UINT32 GetHevcNalType(const BYTE* data, size_t size) {
    if (!data || size < 2) return 0xFFFFFFFFu;
    return (data[0] >> 1) & 0x3f;
}

static std::vector<BYTE> RemoveEmulationPrevention(const BYTE* nal, size_t size) {
    std::vector<BYTE> out;
    if (!nal || size < 2) return out;

    out.reserve(size);
    out.push_back(nal[0]);
    out.push_back(nal[1]);

    int zeros = 0;
    for (size_t i = 2; i < size; ++i) {
        const BYTE b = nal[i];

        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;
        }

        out.push_back(b);
        if (b == 0) {
            zeros = (zeros < 2) ? (zeros + 1) : 2;
        } else {
            zeros = 0;
        }
    }

    return out;
}

static size_t FindStartCode(const BYTE* data, size_t size, size_t from, size_t* codeSize) {
    if (codeSize) *codeSize = 0;
    if (!data || from >= size) return size;

    for (size_t i = from; i + 3 <= size; ++i) {
        if (i + 4 <= size &&
            data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            if (codeSize) *codeSize = 4;
            return i;
        }

        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (codeSize) *codeSize = 3;
            return i;
        }
    }

    return size;
}

static void AddCapturedCustomNal(
    CapturedDvAccessUnit& au,
    const BYTE* nal,
    size_t size) {

    const UINT32 type = GetHevcNalType(nal, size);
    if (type != kDolbyVisionHevcNaluType && type != kDolbyVisionFelNaluType) {
        return;
    }

    CapturedDvUnit unit;
    unit.nalType = type;
    unit.bytes = RemoveEmulationPrevention(nal, size);
    unit.rawBytes.assign(nal, nal + size);

    if (unit.bytes.size() >= 2 && unit.rawBytes.size() >= 2) {
        au.units.push_back(std::move(unit));
    }
}



static double FixedDenominator(uint64_t bits) {
    if (bits > 52) return 0.0;
    return std::ldexp(1.0, static_cast<int>(bits));
}

static bool ReadU64Value(const DoviU64DataC& data, size_t index, uint64_t& out) {
    if (!data.data || index >= data.len) return false;
    out = data.data[index];
    return true;
}

static bool ReadI64Value(const DoviI64DataC& data, size_t index, int64_t& out) {
    if (!data.data || index >= data.len) return false;
    out = data.data[index];
    return true;
}

static bool ReadU64Value2D(const DoviU64Data2DC& data, size_t row, size_t col, uint64_t& out) {
    if (!data.list || row >= data.len || !data.list[row]) return false;
    return ReadU64Value(*data.list[row], col, out);
}

static bool ReadI64Value2D(const DoviI64Data2DC& data, size_t row, size_t col, int64_t& out) {
    if (!data.list || row >= data.len || !data.list[row]) return false;
    return ReadI64Value(*data.list[row], col, out);
}

static bool ReadU64Value3D(
    const DoviU64Data3DC& data,
    size_t plane,
    size_t row,
    size_t col,
    uint64_t& out) {

    if (!data.list || plane >= data.len || !data.list[plane]) return false;
    return ReadU64Value2D(*data.list[plane], row, col, out);
}

static bool ReadI64Value3D(
    const DoviI64Data3DC& data,
    size_t plane,
    size_t row,
    size_t col,
    int64_t& out) {

    if (!data.list || plane >= data.len || !data.list[plane]) return false;
    return ReadI64Value2D(*data.list[plane], row, col, out);
}

static float DecodeSignedCoefficient(
    int64_t integerPart,
    uint64_t fractionalPart,
    double denominator) {

    if (!(denominator > 0.0)) return 0.0f;
    return static_cast<float>(
        static_cast<double>(integerPart) +
        static_cast<double>(fractionalPart) / denominator);
}

static DolbyColorInfo DefaultDolbyColor() {
    DolbyColorInfo out{};
    out.valid = true;

    static const int ycc[9] = {
        9575, 0, 14742,
        9575, 1754, 4383,
        9575, 17372, 0
    };
    static const int lms[9] = {
        5845, 9702, 837,
        2568, 12256, 1561,
        0, 679, 15705
    };
    static const float offsets[3] = { 0.25f, 2.0f, 2.0f };

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const int i = r * 3 + c;
            out.nonlinear[r][c] = static_cast<float>(ycc[i]) / 8192.0f;
            out.linear[r][c] = static_cast<float>(lms[i]) / 16384.0f;
        }
        out.nonlinearOffset[r] = offsets[r];
    }
    return out;
}

static bool CopyDolbyColor(const DoviVdrDmDataPrefixC* dm, DolbyColorInfo& out) {
    if (!dm || dm->compressed) return false;

    out = DolbyColorInfo{};
    out.valid = true;

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const int i = r * 3 + c;
            out.nonlinear[r][c] = static_cast<float>(dm->yccToRgbCoef[i]) / 8192.0f;
            out.linear[r][c] = static_cast<float>(dm->rgbToLmsCoef[i]) / 16384.0f;
        }
        out.nonlinearOffset[r] =
            static_cast<float>(static_cast<double>(dm->yccToRgbOffset[r]) / 268435456.0);
    }

    return true;
}

static DolbyColorInfo GetPreviousDolbyColor() {
    AcquireSRWLockShared(&g_doviColorStateLock);
    DolbyColorInfo out = g_lastDoviColor.valid ? g_lastDoviColor : DefaultDolbyColor();
    ReleaseSRWLockShared(&g_doviColorStateLock);
    return out;
}

static void SetPreviousDolbyColor(const DolbyColorInfo& color) {
    if (!color.valid) return;
    AcquireSRWLockExclusive(&g_doviColorStateLock);
    g_lastDoviColor = color;
    ReleaseSRWLockExclusive(&g_doviColorStateLock);
}

static bool CopyRpuCurve(
    const DoviReshapingCurveC& source,
    uint8_t coefficientDataType,
    double coefficientDenom,
    double pivotDenom,
    RpuCurveInfo& out) {

    out = RpuCurveInfo{};
    if (coefficientDataType != 0 || !(coefficientDenom > 0.0) || !(pivotDenom > 0.0)) {
        return false;
    }

    const uint64_t numPivots64 = source.numPivotsMinus2 + 2;
    if (numPivots64 < 2 || numPivots64 > 9 ||
        !source.pivots.data || source.pivots.len < numPivots64) {
        return false;
    }

    out.numPivots = static_cast<uint32_t>(numPivots64);
    out.mappingIdc = source.mappingIdc;
    for (uint32_t i = 0; i < out.numPivots; ++i) {
        out.pivots[i] = static_cast<float>(
            static_cast<double>(source.pivots.data[i]) / pivotDenom);
    }

    const uint32_t pieces = out.numPivots - 1;
    if (source.mappingIdc == 0) {
        if (!source.polynomial) return false;
        for (uint32_t piece = 0; piece < pieces; ++piece) {
            uint64_t orderMinus1 = 0;
            if (!ReadU64Value(source.polynomial->polyOrderMinus1, piece, orderMinus1)) {
                return false;
            }
            const uint32_t order = static_cast<uint32_t>(orderMinus1 + 1);
            if (order < 1 || order > 2) return false;
            out.polyOrder[piece] = order;

            for (uint32_t k = 0; k <= order; ++k) {
                int64_t integerPart = 0;
                uint64_t fraction = 0;
                if (!ReadI64Value2D(source.polynomial->polyCoefInt, piece, k, integerPart) ||
                    !ReadU64Value2D(source.polynomial->polyCoef, piece, k, fraction)) {
                    return false;
                }
                out.polyCoeff[piece][k] =
                    DecodeSignedCoefficient(integerPart, fraction, coefficientDenom);
            }
        }
        return true;
    }

    if (source.mappingIdc == 1) {
        if (!source.mmr) return false;
        for (uint32_t piece = 0; piece < pieces; ++piece) {
            if (!source.mmr->mmrOrderMinus1.data || piece >= source.mmr->mmrOrderMinus1.len) {
                return false;
            }
            const uint32_t order = static_cast<uint32_t>(source.mmr->mmrOrderMinus1.data[piece]) + 1;
            if (order < 1 || order > 3) return false;
            out.mmrOrder[piece] = order;

            int64_t constantInt = 0;
            uint64_t constantFrac = 0;
            if (!ReadI64Value(source.mmr->mmrConstantInt, piece, constantInt) ||
                !ReadU64Value(source.mmr->mmrConstant, piece, constantFrac)) {
                return false;
            }
            out.mmrConstant[piece] =
                DecodeSignedCoefficient(constantInt, constantFrac, coefficientDenom);

            for (uint32_t j = 0; j < order; ++j) {
                for (uint32_t k = 0; k < 7; ++k) {
                    int64_t integerPart = 0;
                    uint64_t fraction = 0;
                    if (!ReadI64Value3D(source.mmr->mmrCoefInt, piece, j, k, integerPart) ||
                        !ReadU64Value3D(source.mmr->mmrCoef, piece, j, k, fraction)) {
                        return false;
                    }
                    out.mmrCoeff[piece][j][k] =
                        DecodeSignedCoefficient(integerPart, fraction, coefficientDenom);
                }
            }
        }
        return true;
    }

    return false;
}

static bool ParseRpuInfo(const CapturedDvAccessUnit& au, ParsedRpuInfo& out) {
    out = ParsedRpuInfo{};
    if (!LoadDoviApi()) return false;

    const CapturedDvUnit* rpuUnit = nullptr;
    for (const auto& unit : au.units) {
        if (unit.nalType == kDolbyVisionHevcNaluType && unit.rawBytes.size() > 2) {
            rpuUnit = &unit;
            break;
        }
    }
    if (!rpuUnit) return false;

    void* rpu = g_doviParseUnspec62(rpuUnit->rawBytes.data(), rpuUnit->rawBytes.size());
    if (!rpu) return false;

    const DoviRpuDataHeaderC* header = g_doviGetHeader(rpu);
    if (!header) {
        const char* err = g_doviGetError ? g_doviGetError(rpu) : nullptr;
        if (err) Log(L"FEL RPU PARSE header FAILED error=%S", err);
        g_doviRpuFree(rpu);
        return false;
    }

    out.parsed = true;
    out.guessedProfile = header->guessedProfile;
    out.profile7 = out.guessedProfile == 7;
    out.fel = out.profile7 && header->elType && _stricmp(header->elType, "FEL") == 0;
    out.mel = out.profile7 && header->elType && _stricmp(header->elType, "MEL") == 0;
    out.coefficientDataType = header->coefficientDataType;
    out.coefficientLog2Denom = header->coefficientLog2Denom;
    out.blBitDepth = header->blBitDepthMinus8 + 8;
    out.elBitDepth = header->elBitDepthMinus8 + 8;
    out.blVideoFullRange = header->blVideoFullRangeFlag;

    const double coefficientDenom = FixedDenominator(out.coefficientLog2Denom);
    const double pivotDenom = out.blBitDepth > 0 && out.blBitDepth < 31
        ? static_cast<double>((1u << static_cast<unsigned>(out.blBitDepth)) - 1u)
        : 0.0;

    bool mappingReady = false;
    const DoviRpuDataMappingC* mapping = g_doviGetMapping(rpu);
    if (mapping) {
        out.nlqMethodIdc = mapping->nlqMethodIdc;
        if (mapping->nlq) {
            out.nlqActive = !header->disableResidualFlag && mapping->nlqMethodIdc == 0;
            const double elMax = out.elBitDepth > 0 && out.elBitDepth < 31
                ? static_cast<double>((1u << static_cast<unsigned>(out.elBitDepth)) - 1u)
                : 0.0;

            for (int c = 0; c < 3; ++c) {
                out.offset[c] = mapping->nlq->nlqOffset[c];
                out.slopeInt[c] = mapping->nlq->linearDeadzoneSlopeInt[c];
                out.slope[c] = mapping->nlq->linearDeadzoneSlope[c];
                out.thresholdInt[c] = mapping->nlq->linearDeadzoneThresholdInt[c];
                out.threshold[c] = mapping->nlq->linearDeadzoneThreshold[c];

                if (elMax > 0.0 && coefficientDenom > 0.0) {
                    const double combinedSlope =
                        static_cast<double>(out.slopeInt[c]) * coefficientDenom +
                        static_cast<double>(out.slope[c]);
                    const double combinedThreshold =
                        static_cast<double>(out.thresholdInt[c]) * coefficientDenom +
                        static_cast<double>(out.threshold[c]);

                    out.nlqOffsetNorm[c] = static_cast<float>(out.offset[c] / elMax);
                    out.nlqSlopeNorm[c] = static_cast<float>(
                        elMax * combinedSlope / coefficientDenom);
                    out.nlqThresholdNorm[c] = static_cast<float>(
                        (combinedThreshold - 0.5 * combinedSlope) / coefficientDenom);
                }
            }
        }

        mappingReady = out.profile7 && out.fel && out.nlqActive &&
            out.coefficientDataType == 0 && coefficientDenom > 0.0 && pivotDenom > 0.0;

        if (mappingReady) {
            for (int c = 0; c < 3; ++c) {
                if (!CopyRpuCurve(
                        mapping->curves[c],
                        out.coefficientDataType,
                        coefficientDenom,
                        pivotDenom,
                        out.curves[c])) {
                    mappingReady = false;
                    break;
                }
            }
        }
        g_doviFreeMapping(mapping);
    }

    DolbyColorInfo color{};
    const DoviVdrDmDataPrefixC* dm = g_doviGetVdrDm(rpu);
    if (dm) {
        if (CopyDolbyColor(dm, color)) {
            SetPreviousDolbyColor(color);
        } else {
            color = GetPreviousDolbyColor();
        }
        g_doviFreeVdrDm(dm);
    } else {
        color = GetPreviousDolbyColor();
    }
    out.color = color;
    out.renderMetadataReady = mappingReady && color.valid;

    g_doviFreeHeader(header);
    g_doviRpuFree(rpu);
    return out.parsed;
}

static bool BuildElAnnexB(const CapturedDvAccessUnit& au, std::vector<BYTE>& out) {
    out.clear();
    for (const auto& unit : au.units) {
        if (unit.nalType != kDolbyVisionFelNaluType || unit.rawBytes.size() <= 4) {
            continue;
        }

        static const BYTE startCode[4] = {0, 0, 0, 1};
        out.insert(out.end(), startCode, startCode + 4);
        out.insert(out.end(), unit.rawBytes.begin() + 2, unit.rawBytes.end());
    }
    return !out.empty();
}


struct ElStreamInfo {
    bool valid = false;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 profile = 0;
    UINT32 bitDepth = 0;
};

class BitReader {
public:
    BitReader(const BYTE* data, size_t size) : data_(data), bits_(size * 8) {}

    bool ReadBit(UINT32& out) {
        if (pos_ >= bits_) return false;
        out = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u;
        ++pos_;
        return true;
    }

    bool ReadBits(UINT32 count, uint64_t& out) {
        if (count > 64 || pos_ + count > bits_) return false;
        out = 0;
        for (UINT32 i = 0; i < count; ++i) {
            UINT32 b = 0;
            if (!ReadBit(b)) return false;
            out = (out << 1) | b;
        }
        return true;
    }

    bool SkipBits(size_t count) {
        if (pos_ + count > bits_) return false;
        pos_ += count;
        return true;
    }

    bool ReadUE(uint64_t& out) {
        UINT32 b = 0;
        UINT32 zeros = 0;
        while (true) {
            if (!ReadBit(b)) return false;
            if (b) break;
            if (++zeros > 31) return false;
        }
        uint64_t suffix = 0;
        if (zeros && !ReadBits(zeros, suffix)) return false;
        out = ((uint64_t(1) << zeros) - 1) + suffix;
        return true;
    }

private:
    const BYTE* data_ = nullptr;
    size_t bits_ = 0;
    size_t pos_ = 0;
};

static std::vector<BYTE> HevcNalToRbsp(const BYTE* nal, size_t size) {
    std::vector<BYTE> out;
    if (!nal || size <= 2) return out;
    out.reserve(size - 2);
    int zeros = 0;
    for (size_t i = 2; i < size; ++i) {
        const BYTE b = nal[i];
        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        out.push_back(b);
        if (b == 0) zeros = (zeros < 2) ? zeros + 1 : 2;
        else zeros = 0;
    }
    return out;
}

static bool SkipHevcProfileTierLevel(BitReader& br, UINT32 maxSubLayersMinus1, UINT32& generalProfileIdc) {
    uint64_t v = 0;
    if (!br.ReadBits(2, v)) return false;
    if (!br.ReadBits(1, v)) return false;
    if (!br.ReadBits(5, v)) return false;
    generalProfileIdc = static_cast<UINT32>(v);
    if (!br.SkipBits(32 + 4 + 44 + 8)) return false;

    UINT32 profilePresent[8]{};
    UINT32 levelPresent[8]{};
    for (UINT32 i = 0; i < maxSubLayersMinus1; ++i) {
        uint64_t p = 0, l = 0;
        if (!br.ReadBits(1, p) || !br.ReadBits(1, l)) return false;
        profilePresent[i] = static_cast<UINT32>(p);
        levelPresent[i] = static_cast<UINT32>(l);
    }
    if (maxSubLayersMinus1 > 0) {
        for (UINT32 i = maxSubLayersMinus1; i < 8; ++i) {
            if (!br.SkipBits(2)) return false;
        }
    }
    for (UINT32 i = 0; i < maxSubLayersMinus1; ++i) {
        if (profilePresent[i] && !br.SkipBits(2 + 1 + 5 + 32 + 4 + 44)) return false;
        if (levelPresent[i] && !br.SkipBits(8)) return false;
    }
    return true;
}

static bool ParseHevcSps(const BYTE* nal, size_t size, ElStreamInfo& out) {
    if (!nal || size < 6 || GetHevcNalType(nal, size) != 33) return false;
    const auto rbsp = HevcNalToRbsp(nal, size);
    if (rbsp.empty()) return false;

    BitReader br(rbsp.data(), rbsp.size());
    uint64_t v = 0;
    if (!br.ReadBits(4, v)) return false;
    if (!br.ReadBits(3, v)) return false;
    const UINT32 maxSubLayersMinus1 = static_cast<UINT32>(v);
    if (!br.SkipBits(1)) return false;

    UINT32 profileIdc = 0;
    if (!SkipHevcProfileTierLevel(br, maxSubLayersMinus1, profileIdc)) return false;

    uint64_t spsId = 0, chromaFormatIdc = 0;
    if (!br.ReadUE(spsId) || !br.ReadUE(chromaFormatIdc)) return false;
    if (chromaFormatIdc > 3) return false;

    UINT32 separateColourPlane = 0;
    if (chromaFormatIdc == 3) {
        if (!br.ReadBit(separateColourPlane)) return false;
    }

    uint64_t codedWidth = 0, codedHeight = 0;
    if (!br.ReadUE(codedWidth) || !br.ReadUE(codedHeight)) return false;
    if (!codedWidth || !codedHeight || codedWidth > 8192 || codedHeight > 4320) return false;

    UINT32 conformance = 0;
    if (!br.ReadBit(conformance)) return false;
    uint64_t left = 0, right = 0, top = 0, bottom = 0;
    if (conformance) {
        if (!br.ReadUE(left) || !br.ReadUE(right) || !br.ReadUE(top) || !br.ReadUE(bottom)) return false;
    }

    uint64_t bitDepthLumaMinus8 = 0, bitDepthChromaMinus8 = 0;
    if (!br.ReadUE(bitDepthLumaMinus8) || !br.ReadUE(bitDepthChromaMinus8)) return false;

    UINT32 subWidthC = 1, subHeightC = 1;
    if (!separateColourPlane) {
        if (chromaFormatIdc == 1) { subWidthC = 2; subHeightC = 2; }
        else if (chromaFormatIdc == 2) { subWidthC = 2; subHeightC = 1; }
    }

    const uint64_t cropX = subWidthC * (left + right);
    const uint64_t cropY = subHeightC * (top + bottom);
    if (cropX >= codedWidth || cropY >= codedHeight) return false;

    out.valid = true;
    out.width = static_cast<UINT32>(codedWidth - cropX);
    out.height = static_cast<UINT32>(codedHeight - cropY);
    out.profile = profileIdc;
    out.bitDepth = static_cast<UINT32>(8 + bitDepthLumaMinus8);
    return true;
}

static bool GetElStreamInfo(const CapturedDvAccessUnit& au, ElStreamInfo& out) {
    for (const auto& unit : au.units) {
        if (unit.nalType != kDolbyVisionFelNaluType || unit.rawBytes.size() <= 6) continue;
        const BYTE* inner = unit.rawBytes.data() + 2;
        const size_t innerSize = unit.rawBytes.size() - 2;
        if (GetHevcNalType(inner, innerSize) == 33 && ParseHevcSps(inner, innerSize, out)) {
            return true;
        }
    }
    return false;
}


static bool BuildElSequenceHeader(const CapturedDvAccessUnit& au, std::vector<BYTE>& out) {
    out.clear();
    static const BYTE startCode[4] = {0, 0, 0, 1};
    bool haveVps = false, haveSps = false, havePps = false;

    for (const auto& unit : au.units) {
        if (unit.nalType != kDolbyVisionFelNaluType || unit.rawBytes.size() <= 4) continue;

        const BYTE* inner = unit.rawBytes.data() + 2;
        const size_t innerSize = unit.rawBytes.size() - 2;
        const UINT32 type = GetHevcNalType(inner, innerSize);
        if (type != 32 && type != 33 && type != 34) continue;

        if ((type == 32 && haveVps) || (type == 33 && haveSps) || (type == 34 && havePps)) continue;

        out.insert(out.end(), startCode, startCode + 4);
        out.insert(out.end(), inner, inner + innerSize);

        if (type == 32) haveVps = true;
        if (type == 33) haveSps = true;
        if (type == 34) havePps = true;
    }

    return haveSps && havePps;
}

static bool ElAuHasIrap(const CapturedDvAccessUnit& au) {
    for (const auto& unit : au.units) {
        if (unit.nalType != kDolbyVisionFelNaluType || unit.rawBytes.size() <= 4) continue;
        const UINT32 t = GetHevcNalType(unit.rawBytes.data() + 2, unit.rawBytes.size() - 2);
        if (t >= 16 && t <= 23) return true;
    }
    return false;
}

struct DecodedElFrame {
    LONGLONG sampleTime = -1;
    GUID subtype = GUID_NULL;
    UINT32 width = 0;
    UINT32 height = 0;
    uint64_t hash = 0;
    std::vector<BYTE> bytes;
};

static std::deque<DecodedElFrame> g_elFrames;

static const wchar_t* ElSubtypeName(const GUID& subtype) {
    if (subtype == MFVideoFormat_P010) return L"P010";
    if (subtype == MFVideoFormat_NV12) return L"NV12";
    return L"OTHER";
}

static bool CaptureDecodedElFrame(IMFSample* sample, DecodedElFrame& frame) {
    if (!sample) return false;

    frame.sampleTime = -1;
    sample->GetSampleTime(&frame.sampleTime);
    frame.subtype = g_elOutputSubtype;
    frame.width = g_elOutputWidth;
    frame.height = g_elOutputHeight;

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr) || !buffer) return false;

    BYTE* data = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    hr = buffer->Lock(&data, &maxLen, &curLen);
    if (FAILED(hr) || !data || !curLen) {
        if (SUCCEEDED(hr)) buffer->Unlock();
        buffer->Release();
        return false;
    }

    frame.bytes.assign(data, data + curLen);
    uint64_t hash = 1469598103934665603ULL;
    HashUpdate64(hash, data, curLen);
    frame.hash = hash;

    buffer->Unlock();
    buffer->Release();
    return true;
}

static void QueueDecodedElFrame(IMFSample* sample, LONG frameIndex) {
    DecodedElFrame frame;
    if (!CaptureDecodedElFrame(sample, frame)) {
        if (frameIndex <= 120) {
            Log(L"FEL ELDEC CAPTURE frame=%ld FAILED", frameIndex);
        }
        return;
    }

    g_elFrames.push_back(std::move(frame));
    while (g_elFrames.size() > 32) g_elFrames.pop_front();

    const auto& back = g_elFrames.back();
    if (frameIndex <= 120) {
        Log(L"FEL ELDEC FRAME=%ld time=%lld bytes=%llu subtype=%s hash=%016llX queue=%llu",
            frameIndex,
            static_cast<long long>(back.sampleTime),
            static_cast<unsigned long long>(back.bytes.size()),
            ElSubtypeName(back.subtype),
            static_cast<unsigned long long>(back.hash),
            static_cast<unsigned long long>(g_elFrames.size()));
    }
}


static bool ConfigureElDecoderOutputLocked(bool logTypes) {
    if (!g_elDecoder) return false;

    IMFMediaType* p010Type = nullptr;
    IMFMediaType* nv12Type = nullptr;

    for (DWORD i = 0; ; ++i) {
        IMFMediaType* type = nullptr;
        HRESULT typeHr = g_elDecoder->GetOutputAvailableType(0, i, &type);
        if (typeHr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(typeHr) || !type) continue;

        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);

        if (subtype == MFVideoFormat_P010) {
            if (logTypes) Log(L"FEL ELDEC OUTPUT TYPE index=%lu subtype=P010 size=%ux%u", i, w, h);
            if (!p010Type) {
                p010Type = type;
                continue;
            }
        } else if (subtype == MFVideoFormat_NV12) {
            if (logTypes) Log(L"FEL ELDEC OUTPUT TYPE index=%lu subtype=NV12 size=%ux%u", i, w, h);
            if (!nv12Type) {
                nv12Type = type;
                continue;
            }
        }

        type->Release();
    }

    IMFMediaType* chosen = p010Type ? p010Type : nv12Type;
    if (!chosen) {
        if (p010Type) p010Type->Release();
        if (nv12Type) nv12Type->Release();
        Log(L"FEL ELDEC no P010/NV12 output type");
        return false;
    }

    GUID chosenSubtype{};
    chosen->GetGUID(MF_MT_SUBTYPE, &chosenSubtype);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(chosen, MF_MT_FRAME_SIZE, &w, &h);

    HRESULT hr = g_elDecoder->SetOutputType(0, chosen, 0);
    if (FAILED(hr) && chosenSubtype == MFVideoFormat_P010 && nv12Type) {
        Log(L"FEL ELDEC P010 SetOutputType failed hr=0x%08X; trying NV12",
            static_cast<unsigned>(hr));
        chosen = nv12Type;
        chosen->GetGUID(MF_MT_SUBTYPE, &chosenSubtype);
        w = h = 0;
        MFGetAttributeSize(chosen, MF_MT_FRAME_SIZE, &w, &h);
        hr = g_elDecoder->SetOutputType(0, chosen, 0);
    }

    if (p010Type) p010Type->Release();
    if (nv12Type) nv12Type->Release();

    if (FAILED(hr)) {
        Log(L"FEL ELDEC SetOutputType failed hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    g_elOutputSubtype = chosenSubtype;
    if (w && h) {
        g_elOutputWidth = w;
        g_elOutputHeight = h;
    }

    Log(L"FEL ELDEC OUTPUT CONFIGURED output=%s size=%ux%u exact10bit=%s",
        ElSubtypeName(g_elOutputSubtype),
        g_elOutputWidth,
        g_elOutputHeight,
        g_elOutputSubtype == MFVideoFormat_P010 ? L"YES" : L"NO");
    return true;
}

static void DrainElDecoderLocked() {
    if (!g_elDecoder) return;

    MFT_OUTPUT_STREAM_INFO info{};
    if (FAILED(g_elDecoder->GetOutputStreamInfo(0, &info))) return;

    for (;;) {
        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;

        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            if (FAILED(MFCreateSample(&sample))) break;
            const DWORD bytes = info.cbSize ? info.cbSize : (std::max(g_elOutputWidth, 48u) * std::max(g_elOutputHeight, 48u) * 3u);
            HRESULT allocHr = info.cbAlignment
                ? MFCreateAlignedMemoryBuffer(bytes, info.cbAlignment - 1, &buffer)
                : MFCreateMemoryBuffer(bytes, &buffer);
            if (FAILED(allocHr)) {
                sample->Release();
                break;
            }
            sample->AddBuffer(buffer);
            buffer->Release();
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = sample;
        DWORD status = 0;
        HRESULT hr = g_elDecoder->ProcessOutput(0, 1, &output, &status);

        IMFSample* actual = output.pSample ? output.pSample : sample;
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (sample) sample->Release();
            if (output.pEvents) output.pEvents->Release();
            break;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (sample) sample->Release();
            if (output.pEvents) output.pEvents->Release();
            Log(L"FEL ELDEC STREAM_CHANGE; renegotiating output");
            if (!ConfigureElDecoderOutputLocked(true)) break;
            continue;
        }

        if (FAILED(hr)) {
            Log(L"FEL ELDEC ProcessOutput hr=0x%08X", static_cast<unsigned>(hr));
            if (sample) sample->Release();
            if (output.pEvents) output.pEvents->Release();
            break;
        }

        const LONG frame = InterlockedIncrement(&g_elDecodedFrames);
        if (actual) QueueDecodedElFrame(actual, frame);

        if (sample) sample->Release();
        if (output.pEvents) output.pEvents->Release();
    }
}

static bool InitElDecoderLocked(const CapturedDvAccessUnit& au) {
    if (g_elDecoder) return true;
    if (!g_originalMFTEnumEx) return false;

    ElStreamInfo streamInfo{};
    if (!GetElStreamInfo(au, streamInfo)) {
        Log(L"FEL ELDEC WAIT_SPS time=%lld", static_cast<long long>(au.sampleTime));
        return false;
    }

    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_HEVC};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    HRESULT hr = g_originalMFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inInfo,
        nullptr,
        &activates,
        &count);

    if (FAILED(hr) || count == 0) {
        if (activates) {
            CoTaskMemFree(activates);
            activates = nullptr;
        }
        count = 0;
        inInfo.guidSubtype = kHevcEsSubtype;
        hr = g_originalMFTEnumEx(
            MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inInfo,
            nullptr,
            &activates,
            &count);
    }

    if (FAILED(hr) || !activates || !count) {
        Log(L"FEL ELDEC enumerate failed hr=0x%08X count=%u", static_cast<unsigned>(hr), count);
        if (activates) CoTaskMemFree(activates);
        return false;
    }

    IMFTransform* decoder = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
        if (!activates[i]) continue;
        wchar_t name[256]{};
        ReadFriendlyName(activates[i], name, _countof(name));
        HRESULT activateHr = activates[i]->ActivateObject(IID_PPV_ARGS(&decoder));
        if (SUCCEEDED(activateHr) && decoder) {
            Log(L"FEL ELDEC using MFT=%s", name[0] ? name : L"<unnamed>");
            break;
        }
    }

    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i]) activates[i]->Release();
    }
    CoTaskMemFree(activates);

    if (!decoder) {
        Log(L"FEL ELDEC no HEVC decoder could be activated");
        return false;
    }

    IMFAttributes* decoderAttrs = nullptr;
    HRESULT attrsHr = decoder->GetAttributes(&decoderAttrs);
    HRESULT lowLatencyHr = E_NOINTERFACE;
    if (SUCCEEDED(attrsHr) && decoderAttrs) {
        lowLatencyHr = decoderAttrs->SetUINT32(MF_LOW_LATENCY, FALSE);
        decoderAttrs->Release();
    }
    Log(L"FEL ELDEC LOW_LATENCY=OFF attrsHr=0x%08X setHr=0x%08X",
        static_cast<unsigned>(attrsHr),
        static_cast<unsigned>(lowLatencyHr));

    IMFMediaType* inputType = nullptr;
    hr = MFCreateMediaType(&inputType);
    if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_SUBTYPE, inInfo.guidSubtype);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, streamInfo.width, streamInfo.height);
    if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_MPEG2_PROFILE, streamInfo.profile);
    if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_COMPRESSED, TRUE);

    std::vector<BYTE> sequenceHeader;
    const bool haveSequenceHeader = BuildElSequenceHeader(au, sequenceHeader);
    if (SUCCEEDED(hr) && haveSequenceHeader && !sequenceHeader.empty()) {
        hr = inputType->SetBlob(
            MF_MT_MPEG_SEQUENCE_HEADER,
            sequenceHeader.data(),
            static_cast<UINT32>(sequenceHeader.size()));
    }

    Log(L"FEL ELDEC MEDIA TYPE sequenceHeader=%s bytes=%llu",
        haveSequenceHeader ? L"YES" : L"NO",
        static_cast<unsigned long long>(sequenceHeader.size()));

    if (SUCCEEDED(hr)) hr = decoder->SetInputType(0, inputType, 0);

    if (FAILED(hr) && inInfo.guidSubtype == MFVideoFormat_HEVC) {
        Log(L"FEL ELDEC HEVC SetInputType failed hr=0x%08X; trying HEVC_ES",
            static_cast<unsigned>(hr));
        if (inputType) {
            inputType->SetGUID(MF_MT_SUBTYPE, kHevcEsSubtype);
            hr = decoder->SetInputType(0, inputType, 0);
            if (SUCCEEDED(hr)) inInfo.guidSubtype = kHevcEsSubtype;
        }
    }

    if (inputType) inputType->Release();

    Log(L"FEL ELDEC STREAM subtype=%s size=%ux%u profile=%u bitDepth=%u",
        inInfo.guidSubtype == kHevcEsSubtype ? L"HEVC_ES" : L"HEVC",
        streamInfo.width, streamInfo.height, streamInfo.profile, streamInfo.bitDepth);

    if (FAILED(hr)) {
        Log(L"FEL ELDEC SetInputType failed hr=0x%08X", static_cast<unsigned>(hr));
        decoder->Release();
        return false;
    }

    g_elOutputWidth = streamInfo.width;
    g_elOutputHeight = streamInfo.height;
    g_elOutputSubtype = GUID_NULL;

    g_elDecoder = decoder;
    if (!ConfigureElDecoderOutputLocked(true)) {
        Log(L"FEL ELDEC output negotiation failed before streaming; aborting decoder instance");
        g_elDecoder = nullptr;
        decoder->Release();
        return false;
    }

    HRESULT beginHr = decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    HRESULT startHr = decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    Log(L"FEL ELDEC STREAMING beginHr=0x%08X startHr=0x%08X",
        static_cast<unsigned>(beginHr),
        static_cast<unsigned>(startHr));

    if (FAILED(beginHr) || FAILED(startHr)) {
        g_elDecoder = nullptr;
        decoder->Release();
        return false;
    }

    InterlockedExchange(&g_elDecoderInitState, 2);
    Log(L"FEL ELDEC READY output=%s size=%ux%u exact10bit=%s",
        ElSubtypeName(g_elOutputSubtype),
        g_elOutputWidth,
        g_elOutputHeight,
        g_elOutputSubtype == MFVideoFormat_P010 ? L"YES" : L"NO");
    return true;
}

static void FeedElDecoder(const CapturedDvAccessUnit& au) {
    std::vector<BYTE> annexB;
    if (!BuildElAnnexB(au, annexB)) return;

    AcquireSRWLockExclusive(&g_elDecoderLock);
    if (!g_elDecoder && !InitElDecoderLocked(au)) {
        ReleaseSRWLockExclusive(&g_elDecoderLock);
        return;
    }

    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(static_cast<DWORD>(annexB.size()), &buffer);

    if (SUCCEEDED(hr)) {
        BYTE* dst = nullptr;
        DWORD maxLen = 0;
        DWORD curLen = 0;
        hr = buffer->Lock(&dst, &maxLen, &curLen);
        if (SUCCEEDED(hr)) {
            memcpy(dst, annexB.data(), annexB.size());
            buffer->Unlock();
            hr = buffer->SetCurrentLength(static_cast<DWORD>(annexB.size()));
        }
    }

    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    if (SUCCEEDED(hr) && au.sampleTime >= 0) sample->SetSampleTime(au.sampleTime);
    if (SUCCEEDED(hr) && ElAuHasIrap(au)) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (buffer) buffer->Release();

    if (SUCCEEDED(hr)) {
        const LONG input = InterlockedIncrement(&g_elDecodeInputs);
        hr = g_elDecoder->ProcessInput(0, sample, 0);
        if (input <= 120 || FAILED(hr)) {
            Log(L"FEL ELDEC INPUT=%ld time=%lld bytes=%llu mode=%s hr=0x%08X",
                input,
                static_cast<long long>(au.sampleTime),
                static_cast<unsigned long long>(annexB.size()),
                L"FRAME_ALIGNED_HEVC",
                static_cast<unsigned>(hr));
        }
        if (hr == MF_E_NOTACCEPTING) {
            DrainElDecoderLocked();
            hr = g_elDecoder->ProcessInput(0, sample, 0);
        }
        if (SUCCEEDED(hr)) {
            DrainElDecoderLocked();
        }
    } else {
        Log(L"FEL ELDEC sample creation failed hr=0x%08X", static_cast<unsigned>(hr));
    }

    if (sample) sample->Release();
    ReleaseSRWLockExclusive(&g_elDecoderLock);
}

static bool CaptureDolbyUnits(IMFSample* sample) {
    if (!sample) return false;

    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) {
        return false;
    }

    BYTE* data = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    HRESULT hr = buffer->Lock(&data, &maxLen, &curLen);
    if (FAILED(hr) || !data || curLen < 6) {
        if (SUCCEEDED(hr)) buffer->Unlock();
        buffer->Release();
        return false;
    }

    CapturedDvAccessUnit au;
    sample->GetSampleTime(&au.sampleTime);

    size_t firstScSize = 0;
    const size_t firstSc = FindStartCode(data, curLen, 0, &firstScSize);

    if (firstSc < curLen && firstSc < 64) {
        size_t pos = firstSc;

        while (pos < curLen) {
            size_t scSize = 0;
            const size_t sc = FindStartCode(data, curLen, pos, &scSize);
            if (sc >= curLen) break;

            const size_t nalStart = sc + scSize;
            size_t nextScSize = 0;
            const size_t next = FindStartCode(data, curLen, nalStart, &nextScSize);
            const size_t nalEnd = next < curLen ? next : curLen;

            if (nalEnd > nalStart + 1) {
                AddCapturedCustomNal(au, data + nalStart, nalEnd - nalStart);
            }

            if (next >= curLen) break;
            pos = next;
        }
    }
    else {
        size_t pos = 0;
        while (pos + 4 <= curLen) {
            const uint32_t n =
                (static_cast<uint32_t>(data[pos]) << 24) |
                (static_cast<uint32_t>(data[pos + 1]) << 16) |
                (static_cast<uint32_t>(data[pos + 2]) << 8) |
                static_cast<uint32_t>(data[pos + 3]);

            pos += 4;
            if (n < 2 || pos + n > curLen) break;

            AddCapturedCustomNal(au, data + pos, n);
            pos += n;
        }
    }

    buffer->Unlock();
    buffer->Release();

    UINT32 rpuCount = 0;
    UINT32 elCount = 0;
    size_t elBytes = 0;

    for (const auto& u : au.units) {
        if (u.nalType == kDolbyVisionHevcNaluType) ++rpuCount;
        if (u.nalType == kDolbyVisionFelNaluType) {
            ++elCount;
            elBytes += u.bytes.size();
        }
    }

    if (au.units.empty()) return false;

    if (rpuCount > 0 && ParseRpuInfo(au, au.rpu)) {
        const LONG parsed = InterlockedIncrement(&g_rpuParsedFrames);
        if (au.rpu.fel) InterlockedIncrement(&g_rpuFelFrames);
        if (au.rpu.nlqActive) InterlockedIncrement(&g_rpuNlqFrames);
        if (parsed <= 120) {
            Log(L"FEL RPU FRAME=%ld time=%lld profile=%u profile7=%s el=%s nlq=%s method=%d denom=%llu elBits=%llu offset=%u,%u,%u slope=%llu,%llu,%llu threshold=%llu,%llu,%llu",
                parsed,
                static_cast<long long>(au.sampleTime),
                static_cast<unsigned>(au.rpu.guessedProfile),
                au.rpu.profile7 ? L"YES" : L"NO",
                au.rpu.fel ? L"FEL" : (au.rpu.mel ? L"MEL" : L"OTHER"),
                au.rpu.nlqActive ? L"YES" : L"NO",
                au.rpu.nlqMethodIdc,
                static_cast<unsigned long long>(au.rpu.coefficientLog2Denom),
                static_cast<unsigned long long>(au.rpu.elBitDepth),
                au.rpu.offset[0], au.rpu.offset[1], au.rpu.offset[2],
                static_cast<unsigned long long>(au.rpu.slope[0]),
                static_cast<unsigned long long>(au.rpu.slope[1]),
                static_cast<unsigned long long>(au.rpu.slope[2]),
                static_cast<unsigned long long>(au.rpu.threshold[0]),
                static_cast<unsigned long long>(au.rpu.threshold[1]),
                static_cast<unsigned long long>(au.rpu.threshold[2]));
        }
    }

    const bool exactFel =
        elCount > 0 && au.rpu.parsed && au.rpu.profile7 && au.rpu.fel && au.rpu.nlqActive;

    if (!exactFel) return false;

    if (g_felBridgeEnabled) {
        FeedElDecoder(au);
    }

    const LONGLONG capturedTime = au.sampleTime;

    AcquireSRWLockExclusive(&g_felLock);
    g_dvQueue.push_back(std::move(au));
    while (g_dvQueue.size() > 240) {
        g_dvQueue.pop_front();
    }
    const size_t queueSize = g_dvQueue.size();
    ReleaseSRWLockExclusive(&g_felLock);

    InterlockedExchange(&g_felContentActive, 1);
    const LONG capture = InterlockedIncrement(&g_felCaptureCount);
    if (capture <= 24) {
        Log(L"FEL BRIDGE capture=%ld time=%lld RPU62=%u EL63=%u EL63bytes=%llu queue=%llu",
            capture,
            static_cast<long long>(capturedTime),
            rpuCount,
            elCount,
            static_cast<unsigned long long>(elBytes),
            static_cast<unsigned long long>(queueSize));
    }
    return true;
}

static bool ReadUnitBytes(IUnknown* unknown, std::vector<BYTE>& bytes, UINT32& nalType) {
    bytes.clear();
    nalType = 0xFFFFFFFFu;
    if (!unknown) return false;

    IMFSample* sample = nullptr;
    HRESULT hr = unknown->QueryInterface(IID_PPV_ARGS(&sample));
    if (FAILED(hr) || !sample) return false;

    IMFMediaBuffer* buffer = nullptr;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr) || !buffer) {
        sample->Release();
        return false;
    }

    BYTE* data = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    hr = buffer->Lock(&data, &maxLen, &curLen);

    if (SUCCEEDED(hr) && data && curLen >= 2) {
        bytes.assign(data, data + curLen);
        nalType = GetHevcNalType(data, curLen);
        buffer->Unlock();
        buffer->Release();
        sample->Release();
        return true;
    }

    if (SUCCEEDED(hr)) buffer->Unlock();
    buffer->Release();
    sample->Release();
    return false;
}

static bool SameBytes(const std::vector<BYTE>& a, const std::vector<BYTE>& b) {
    return a.size() == b.size() &&
        (a.empty() || memcmp(a.data(), b.data(), a.size()) == 0);
}

static bool AuHasFel63(const CapturedDvAccessUnit& au) {
    for (const auto& unit : au.units) {
        if (unit.nalType == kDolbyVisionFelNaluType) {
            return true;
        }
    }
    return false;
}

static bool AuContainsExactRpu(
    const CapturedDvAccessUnit& au,
    const std::vector<BYTE>& forwardedRpu) {

    for (const auto& unit : au.units) {
        if (unit.nalType == kDolbyVisionHevcNaluType &&
            SameBytes(unit.bytes, forwardedRpu)) {
            return true;
        }
    }
    return false;
}

static bool TakeCapturedAuForOutput(
    LONGLONG outputTime,
    const std::vector<BYTE>& forwardedRpu,
    CapturedDvAccessUnit& outAu,
    bool& matchedByTime) {

    matchedByTime = false;
    AcquireSRWLockExclusive(&g_felLock);

    if (outputTime >= 0) {
        for (auto it = g_dvQueue.begin(); it != g_dvQueue.end(); ++it) {
            if (it->sampleTime == outputTime && AuHasFel63(*it)) {
                outAu = std::move(*it);
                g_dvQueue.erase(it);
                matchedByTime = true;
                ReleaseSRWLockExclusive(&g_felLock);
                return true;
            }
        }
    }

    if (!forwardedRpu.empty()) {
        for (auto it = g_dvQueue.begin(); it != g_dvQueue.end(); ++it) {
            if (AuHasFel63(*it) && AuContainsExactRpu(*it, forwardedRpu)) {
                outAu = std::move(*it);
                g_dvQueue.erase(it);
                ReleaseSRWLockExclusive(&g_felLock);
                return true;
            }
        }
    }

    ReleaseSRWLockExclusive(&g_felLock);
    return false;
}

static HRESULT CreateForwardedNalSample(
    const CapturedDvUnit& unit,
    LONGLONG sampleTime,
    LONGLONG sampleDuration,
    IMFSample** outSample) {

    if (!outSample) return E_POINTER;
    *outSample = nullptr;
    if (unit.bytes.size() < 2) return E_INVALIDARG;

    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;

    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;

    hr = MFCreateMemoryBuffer(static_cast<DWORD>(unit.bytes.size()), &buffer);
    if (SUCCEEDED(hr)) {
        BYTE* dst = nullptr;
        DWORD maxLen = 0;
        DWORD curLen = 0;

        hr = buffer->Lock(&dst, &maxLen, &curLen);
        if (SUCCEEDED(hr)) {
            memcpy(dst, unit.bytes.data(), unit.bytes.size());
            buffer->Unlock();
            hr = buffer->SetCurrentLength(static_cast<DWORD>(unit.bytes.size()));
        }
    }

    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    if (SUCCEEDED(hr)) hr = sample->SetUINT32(kForwardedDecodeUnitType, kDecodeUnitNal);

    if (SUCCEEDED(hr) && sampleTime >= 0) {
        sample->SetSampleTime(sampleTime);
    }
    if (SUCCEEDED(hr) && sampleDuration > 0) {
        sample->SetSampleDuration(sampleDuration);
    }

    if (buffer) buffer->Release();

    if (FAILED(hr)) {
        sample->Release();
        return hr;
    }

    *outSample = sample;
    return S_OK;
}

static HRESULT AppendSyntheticUnit(
    IMFCollection* collection,
    const CapturedDvUnit& unit,
    LONGLONG sampleTime,
    LONGLONG sampleDuration) {

    IMFSample* synthetic = nullptr;
    HRESULT hr = CreateForwardedNalSample(unit, sampleTime, sampleDuration, &synthetic);
    if (FAILED(hr) || !synthetic) return hr;

    hr = collection->AddElement(synthetic);
    synthetic->Release();
    return hr;
}


static bool FindElFrameByTime(
    LONGLONG sampleTime,
    DecodedElFrame& out,
    LONGLONG* newestTime = nullptr,
    size_t* queuedFrames = nullptr) {

    if (newestTime) *newestTime = -1;
    if (queuedFrames) *queuedFrames = 0;
    if (sampleTime < 0) return false;

    AcquireSRWLockShared(&g_elDecoderLock);
    bool found = false;
    if (queuedFrames) *queuedFrames = g_elFrames.size();
    if (newestTime && !g_elFrames.empty()) {
        *newestTime = g_elFrames.back().sampleTime;
    }
    for (auto it = g_elFrames.rbegin(); it != g_elFrames.rend(); ++it) {
        if (it->sampleTime == sampleTime) {
            out = *it;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_elDecoderLock);
    return found;
}

static const wchar_t* DxgiFormatName(DXGI_FORMAT fmt) {
    if (fmt == DXGI_FORMAT_P010) return L"P010";
    if (fmt == DXGI_FORMAT_NV12) return L"NV12";
    if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) return L"R10G10B10A2";
    return L"OTHER";
}

static bool ReadBlDxgiSurface(IMFSample* sample, DXGI_FORMAT& format, UINT& width, UINT& height, UINT& subresource) {
    format = DXGI_FORMAT_UNKNOWN;
    width = height = subresource = 0;
    if (!sample) return false;

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr) || !buffer) return false;

    IMFDXGIBuffer* dxgiBuffer = nullptr;
    hr = buffer->QueryInterface(IID_PPV_ARGS(&dxgiBuffer));
    buffer->Release();
    if (FAILED(hr) || !dxgiBuffer) return false;

    ID3D11Texture2D* texture = nullptr;
    hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
    if (SUCCEEDED(hr) && texture) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        format = desc.Format;
        width = desc.Width;
        height = desc.Height;
        dxgiBuffer->GetSubresourceIndex(&subresource);
        texture->Release();
    }
    dxgiBuffer->Release();
    return SUCCEEDED(hr) && width && height;
}


struct FelNlqCoeff {
    float offsetCode = 0.0f;
    float slope = 0.0f;
    float threshold = 0.0f;
};

static bool BuildFelNlqCoefficients(const ParsedRpuInfo& rpu, FelNlqCoeff out[3]) {
    if (!rpu.parsed || !rpu.profile7 || !rpu.fel || !rpu.nlqActive) return false;
    if (rpu.nlqMethodIdc != 0 || rpu.coefficientLog2Denom > 62 || rpu.elBitDepth == 0) return false;

    const double denom = std::ldexp(1.0, static_cast<int>(rpu.coefficientLog2Denom));
    if (!(denom > 0.0)) return false;

    for (int c = 0; c < 3; ++c) {
        double slopeFixed = static_cast<double>(rpu.slope[c]);
        double thresholdFixed = static_cast<double>(rpu.threshold[c]);

        if (rpu.coefficientDataType == 0) {
            slopeFixed += static_cast<double>(rpu.slopeInt[c]) * denom;
            thresholdFixed += static_cast<double>(rpu.thresholdInt[c]) * denom;
        }

        out[c].offsetCode = static_cast<float>(rpu.offset[c]);
        out[c].slope = static_cast<float>(slopeFixed / denom);
        out[c].threshold = static_cast<float>(thresholdFixed / denom);
    }

    return true;
}

static inline UINT16 ReadP010Code(const UINT16* words, size_t index) {
    return static_cast<UINT16>(words[index] >> 6);
}

static inline void WriteP010Code(UINT16* words, size_t index, UINT16 code) {
    words[index] = static_cast<UINT16>(code << 6);
}

static inline float SampleEl2xComponent(
    const DecodedElFrame& el,
    int component,
    UINT dstX,
    UINT dstY) {

    if (el.subtype != MFVideoFormat_P010 || el.width < 2 || el.height < 2 || el.bytes.empty()) {
        return 0.0f;
    }

    const UINT16* words = reinterpret_cast<const UINT16*>(el.bytes.data());
    const size_t totalWords = el.bytes.size() / sizeof(UINT16);

    UINT planeWidth = el.width;
    UINT planeHeight = el.height;
    size_t planeBase = 0;
    size_t rowWords = el.width;
    UINT componentOffset = 0;
    UINT componentStep = 1;

    if (component != 0) {
        planeWidth = el.width / 2u;
        planeHeight = el.height / 2u;
        planeBase = static_cast<size_t>(el.width) * el.height;
        rowWords = el.width;
        componentOffset = component == 1 ? 0u : 1u;
        componentStep = 2u;
    }

    if (!planeWidth || !planeHeight || planeBase >= totalWords) return 0.0f;

    UINT x0 = dstX >> 1;
    UINT x1 = x0 + 1;
    float tx = (dstX & 1u) ? 0.5f : 0.0f;
    if (x0 >= planeWidth) x0 = planeWidth - 1;
    if (x1 >= planeWidth) x1 = planeWidth - 1;

    int y0;
    float ty;
    const int halfY = static_cast<int>(dstY >> 1);
    if ((dstY & 1u) == 0) {
        y0 = halfY - 1;
        ty = 0.75f;
    } else {
        y0 = halfY;
        ty = 0.25f;
    }

    int y1 = y0 + 1;
    if (y0 < 0) {
        y0 = 0;
        y1 = 0;
        ty = 0.0f;
    }
    if (y0 >= static_cast<int>(planeHeight)) y0 = static_cast<int>(planeHeight) - 1;
    if (y1 >= static_cast<int>(planeHeight)) y1 = static_cast<int>(planeHeight) - 1;

    const size_t i00 = planeBase + static_cast<size_t>(y0) * rowWords +
        static_cast<size_t>(x0) * componentStep + componentOffset;
    const size_t i10 = planeBase + static_cast<size_t>(y0) * rowWords +
        static_cast<size_t>(x1) * componentStep + componentOffset;
    const size_t i01 = planeBase + static_cast<size_t>(y1) * rowWords +
        static_cast<size_t>(x0) * componentStep + componentOffset;
    const size_t i11 = planeBase + static_cast<size_t>(y1) * rowWords +
        static_cast<size_t>(x1) * componentStep + componentOffset;

    if (i00 >= totalWords || i10 >= totalWords || i01 >= totalWords || i11 >= totalWords) {
        return 0.0f;
    }

    const float a = static_cast<float>(ReadP010Code(words, i00));
    const float b = static_cast<float>(ReadP010Code(words, i10));
    const float c = static_cast<float>(ReadP010Code(words, i01));
    const float d = static_cast<float>(ReadP010Code(words, i11));

    const float top = a + (b - a) * tx;
    const float bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

static inline UINT16 ComposeFelCode(
    UINT16 blCode,
    float elCode,
    const FelNlqCoeff& nlq) {

    const float rr = elCode - nlq.offsetCode;
    if (rr == 0.0f) return blCode;

    const float magnitude = std::fabs(rr);
    const float residual = ((magnitude - 0.5f) * nlq.slope + nlq.threshold) *
        (rr < 0.0f ? -1.0f : 1.0f);

    const float outCode = static_cast<float>(blCode) + residual * 1023.0f;
    const long rounded = std::lround(outCode);
    return static_cast<UINT16>(std::clamp<long>(rounded, 0, 1023));
}

static HRESULT ReplaceSampleVideoBuffer(IMFSample* sample, IMFMediaBuffer* replacement) {
    if (!sample || !replacement) return E_POINTER;

    DWORD count = 0;
    HRESULT hr = sample->GetBufferCount(&count);
    if (FAILED(hr)) return hr;

    std::vector<IMFMediaBuffer*> oldBuffers;
    oldBuffers.reserve(count);

    for (DWORD i = 0; i < count; ++i) {
        IMFMediaBuffer* old = nullptr;
        if (SUCCEEDED(sample->GetBufferByIndex(i, &old)) && old) {
            oldBuffers.push_back(old);
        }
    }

    hr = sample->RemoveAllBuffers();
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(replacement);

    if (FAILED(hr)) {
        sample->RemoveAllBuffers();
        for (IMFMediaBuffer* old : oldBuffers) {
            sample->AddBuffer(old);
        }
    }

    for (IMFMediaBuffer* old : oldBuffers) old->Release();
    return hr;
}

static bool GetSampleDxgiTexture(
    IMFSample* sample,
    ID3D11Texture2D** texture,
    UINT* subresource) {

    if (texture) *texture = nullptr;
    if (subresource) *subresource = 0;
    if (!sample || !texture) return false;

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr) || !buffer) return false;

    IMFDXGIBuffer* dxgi = nullptr;
    hr = buffer->QueryInterface(IID_PPV_ARGS(&dxgi));
    buffer->Release();
    if (FAILED(hr) || !dxgi) return false;

    ID3D11Texture2D* tex = nullptr;
    hr = dxgi->GetResource(IID_PPV_ARGS(&tex));
    if (SUCCEEDED(hr) && tex && subresource) {
        dxgi->GetSubresourceIndex(subresource);
    }

    dxgi->Release();

    if (FAILED(hr) || !tex) {
        if (tex) tex->Release();
        return false;
    }

    *texture = tex;
    return true;
}



struct GpuFelConstants {
    UINT blWidth = 0;
    UINT blHeight = 0;
    UINT elWidth = 0;
    UINT elHeight = 0;
    UINT carrierMode = 1;
    UINT splitMode = 0;
    UINT carrierPad[2]{};
    float offsetCode[4]{};
    float slope[4]{};
    float threshold[4]{};
};

static SRWLOCK g_gpuFelLock = SRWLOCK_INIT;
static ID3D11Device* g_gpuFelDevice = nullptr;
static ID3D11Device3* g_gpuFelDevice3 = nullptr;

static ID3D11Device* g_postDolbyDevice = nullptr;
static ID3D11DeviceContext* g_postDolbyContext = nullptr;
static ID3D11Texture2D* g_postDolbyReadback = nullptr;
static UINT g_postDolbyReadbackWidth = 0;
static UINT g_postDolbyReadbackHeight = 0;

struct PostDolbyRgbConstants {
    UINT width = 0;
    UINT height = 0;
    UINT elWidth = 0;
    UINT elHeight = 0;
    UINT splitMode = 0;
    UINT lumaGuardMode = 1;
    UINT hdr10Available = 0;
    UINT paddingSplit = 0;
    float offsetCode[4]{};
    float slope[4]{};
    float threshold[4]{};
    float nonlinear0[4]{};
    float nonlinear1[4]{};
    float nonlinear2[4]{};
    float post0[4]{};
    float post1[4]{};
    float post2[4]{};
    float invPost0[4]{};
    float invPost1[4]{};
    float invPost2[4]{};
};

static ID3D11Device* g_postRgbDevice = nullptr;
static ID3D11Device3* g_postRgbDevice3 = nullptr;
static ID3D11Texture2D* g_postRgbInput = nullptr;
static ID3D11Texture2D* g_postRgbOutput = nullptr;
static ID3D11Texture2D* g_postRgbElInput = nullptr;
static ID3D11ShaderResourceView* g_postRgbInputSrv = nullptr;
static ID3D11ShaderResourceView1* g_postRgbElYSrv = nullptr;
static ID3D11Texture2D* g_postRgbHdr10Input = nullptr;
static ID3D11ShaderResourceView1* g_postRgbHdr10YSrv = nullptr;
static ID3D11ShaderResourceView1* g_postRgbHdr10UvSrv = nullptr;
static ID3D11ShaderResourceView1* g_postRgbElUvSrv = nullptr;
static ID3D11UnorderedAccessView* g_postRgbOutputUav = nullptr;
static ID3D11ComputeShader* g_postRgbShader = nullptr;
static ID3D11Buffer* g_postRgbConstants = nullptr;

static ID3D11Buffer* g_postRgbClipStats = nullptr;
static ID3D11UnorderedAccessView* g_postRgbClipStatsUav = nullptr;
static ID3D11Buffer* g_postRgbClipStatsReadback = nullptr;
static UINT g_postRgbClipWindowFrames = 0;
static UINT g_postRgbClipWindowsLogged = 0;

static UINT g_postRgbWidth = 0;
static UINT g_postRgbHeight = 0;
static UINT g_postRgbElWidth = 0;
static UINT g_postRgbElHeight = 0;
static ID3D11Texture2D* g_gpuBlInput = nullptr;
static ID3D11Texture2D* g_gpuBlOutput = nullptr;
static ID3D11Texture2D* g_gpuElInput = nullptr;
static ID3D11ShaderResourceView1* g_gpuBlYSrv = nullptr;
static ID3D11ShaderResourceView1* g_gpuBlUvSrv = nullptr;
static ID3D11ShaderResourceView1* g_gpuElYSrv = nullptr;
static ID3D11ShaderResourceView1* g_gpuElUvSrv = nullptr;
static ID3D11UnorderedAccessView1* g_gpuOutYUav = nullptr;
static ID3D11UnorderedAccessView1* g_gpuOutUvUav = nullptr;
static ID3D11Buffer* g_gpuFelConstants = nullptr;
static ID3D11ComputeShader* g_gpuFelShader = nullptr;
static ID3D11ComputeShader* g_gpuFelP016Shader = nullptr;
static UINT g_gpuFelBlWidth = 0;
static UINT g_gpuFelBlHeight = 0;
static UINT g_gpuFelElWidth = 0;
static UINT g_gpuFelElHeight = 0;
static DXGI_FORMAT g_gpuFelFormat = DXGI_FORMAT_UNKNOWN;
static volatile LONG g_referenceCaptureDone = 0;
static uint64_t g_referenceInitialElHash = 0;

static bool HookSiblingPath(const wchar_t* name, wchar_t* out, size_t chars) {
    if (!name || !out || !chars || !g_selfModule) return false;
    out[0] = L'\0';
    if (!GetModuleFileNameW(g_selfModule, out, static_cast<DWORD>(chars))) return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';
    return wcscat_s(out, chars, name) == 0;
}

static bool WriteBinarySibling(const wchar_t* name, const void* data, size_t size) {
    wchar_t path[32768]{};
    if (!HookSiblingPath(name, path, _countof(path))) return false;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const BYTE* p = static_cast<const BYTE*>(data);
    size_t left = size;
    bool ok = true;
    while (left) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 1u << 30));
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote != chunk) { ok = false; break; }
        p += chunk; left -= chunk;
    }
    CloseHandle(h);
    return ok;
}

static bool DumpP010TextureTight(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, const wchar_t* name) {
    if (!device || !context || !source || !name) return false;
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_P010 || !d.Width || !d.Height) return false;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.ArraySize = 1;
    sd.MipLevels = 1;
    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr) || !staging) return false;
    context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE map{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { staging->Release(); return false; }
    const size_t rowBytes = static_cast<size_t>(d.Width) * 2u;
    const size_t yRows = d.Height;
    const size_t uvRows = d.Height / 2u;
    std::vector<BYTE> tight(rowBytes * (yRows + uvRows));
    BYTE* out = tight.data();
    const BYTE* in = static_cast<const BYTE*>(map.pData);
    for (size_t y = 0; y < yRows + uvRows; ++y) {
        memcpy(out + y * rowBytes, in + y * map.RowPitch, rowBytes);
    }
    context->Unmap(staging, 0);
    staging->Release();
    return WriteBinarySibling(name, tight.data(), tight.size());
}


static bool CountP010Low6(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    uint64_t* nonzero,
    uint64_t* total) {

    if (nonzero) *nonzero = 0;
    if (total) *total = 0;
    if (!device || !context || !source || !nonzero || !total) return false;

    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_P010 || !d.Width || !d.Height) return false;

    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.ArraySize = 1;
    sd.MipLevels = 1;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr) || !staging) return false;

    context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE map{};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) {
        staging->Release();
        return false;
    }

    const UINT rows = d.Height + d.Height / 2u;
    uint64_t nz = 0, all = 0;
    for (UINT y = 0; y < rows; ++y) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(
            static_cast<const BYTE*>(map.pData) + static_cast<size_t>(y) * map.RowPitch);
        for (UINT x = 0; x < d.Width; ++x) {
            const uint16_t v = row[x];
            if ((v & 0x003Fu) != 0) ++nz;
            ++all;
        }
    }

    context->Unmap(staging, 0);
    staging->Release();
    *nonzero = nz;
    *total = all;
    return true;
}

static void WriteReferenceMetadata(LONGLONG sampleTime, const D3D11_TEXTURE2D_DESC& bl, const DecodedElFrame& el, const FelNlqCoeff nlq[3]) {
    wchar_t path[32768]{};
    if (!HookSiblingPath(L"FEL-REF-meta.txt", path, _countof(path))) return;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char text[2048]{};
    int n = _snprintf_s(text, _countof(text), _TRUNCATE,
        "version=1.0-beta\r\ncarrier=P010_LOW6_12BIT\r\npts100ns=%lld\r\nBL=%ux%u P010\r\nEL=%ux%u P010\r\nELhash=%016llX\r\nsampling=LIBPLACEBO_EXACT_LUT16_NATIVE_P010\r\noffset=%.9f,%.9f,%.9f\r\nslope=%.9f,%.9f,%.9f\r\nthreshold=%.9f,%.9f,%.9f\r\n",
        static_cast<long long>(sampleTime), bl.Width, bl.Height, el.width, el.height,
        static_cast<unsigned long long>(el.hash),
        nlq[0].offsetCode, nlq[1].offsetCode, nlq[2].offsetCode,
        nlq[0].slope, nlq[1].slope, nlq[2].slope,
        nlq[0].threshold, nlq[1].threshold, nlq[2].threshold);
    DWORD wrote = 0;
    if (n > 0) WriteFile(h, text, static_cast<DWORD>(n), &wrote, nullptr);
    CloseHandle(h);
}

static void ReleaseGpuFelState() {
    if (g_gpuFelP016Shader) { g_gpuFelP016Shader->Release(); g_gpuFelP016Shader = nullptr; }
    if (g_gpuFelShader) { g_gpuFelShader->Release(); g_gpuFelShader = nullptr; }
    if (g_gpuFelConstants) { g_gpuFelConstants->Release(); g_gpuFelConstants = nullptr; }
    if (g_gpuOutUvUav) { g_gpuOutUvUav->Release(); g_gpuOutUvUav = nullptr; }
    if (g_gpuOutYUav) { g_gpuOutYUav->Release(); g_gpuOutYUav = nullptr; }
    if (g_gpuElUvSrv) { g_gpuElUvSrv->Release(); g_gpuElUvSrv = nullptr; }
    if (g_gpuElYSrv) { g_gpuElYSrv->Release(); g_gpuElYSrv = nullptr; }
    if (g_gpuBlUvSrv) { g_gpuBlUvSrv->Release(); g_gpuBlUvSrv = nullptr; }
    if (g_gpuBlYSrv) { g_gpuBlYSrv->Release(); g_gpuBlYSrv = nullptr; }
    if (g_gpuElInput) { g_gpuElInput->Release(); g_gpuElInput = nullptr; }
    if (g_gpuBlOutput) { g_gpuBlOutput->Release(); g_gpuBlOutput = nullptr; }
    if (g_gpuBlInput) { g_gpuBlInput->Release(); g_gpuBlInput = nullptr; }
    if (g_gpuFelDevice3) { g_gpuFelDevice3->Release(); g_gpuFelDevice3 = nullptr; }
    if (g_gpuFelDevice) { g_gpuFelDevice->Release(); g_gpuFelDevice = nullptr; }

    g_gpuFelBlWidth = 0;
    g_gpuFelBlHeight = 0;
    g_gpuFelElWidth = 0;
    g_gpuFelElHeight = 0;
    g_gpuFelFormat = DXGI_FORMAT_UNKNOWN;
    InterlockedExchange(&g_gpuRendererReady, 0);
}

static const char* kFelComputeShader = R"HLSL(
cbuffer FelConstants : register(b0)
{
    uint blWidth;
    uint blHeight;
    uint elWidth;
    uint elHeight;
    uint carrierMode;
    uint splitMode;
    uint2 carrierPad;
    float4 offsetCode;
    float4 slope;
    float4 threshold;
};

Texture2D<float>  blY  : register(t0);
Texture2D<float2> blUV : register(t1);
Texture2D<float>  elY  : register(t2);
Texture2D<float2> elUV : register(t3);

RWTexture2D<float>  outY  : register(u0);
RWTexture2D<float2> outUV : register(u1);

float wxTap(uint parity, int tap)
{
    if (parity == 0) {
        if (tap == -2) return 0.0;
        if (tap == -1) return 0.0;
        if (tap ==  0) return 1.0;
        if (tap ==  1) return 0.0;
        if (tap ==  2) return 0.0;
        return 0.0;
    }

    if (tap == -2) return  0.0244598389;
    if (tap == -1) return -0.1358642578;
    if (tap ==  0) return  0.6115722656;
    if (tap ==  1) return  0.6115722656;
    if (tap ==  2) return -0.1358642578;
    return 0.0244598389;
}

float wyTap(uint parity, int tap)
{
    if (parity == 0) {
        if (tap == -2) return  0.0073785782;
        if (tap == -1) return -0.0679931641;
        if (tap ==  0) return  0.2709350586;
        if (tap ==  1) return  0.8928222656;
        if (tap ==  2) return -0.1333007813;
        return 0.0301132202;
    }

    if (tap == -2) return  0.0301132202;
    if (tap == -1) return -0.1333007813;
    if (tap ==  0) return  0.8928222656;
    if (tap ==  1) return  0.2709350586;
    if (tap ==  2) return -0.0679931641;
    return 0.0073785782;
}

float fp16Round(float v)
{
    return f16tof32(f32tof16(v));
}

float2 fp16Round2(float2 v)
{
    return float2(fp16Round(v.x), fp16Round(v.y));
}

float verticalElYNorm(int sx, uint dstY)
{
    int h = int(elHeight);
    int baseY = int(dstY >> 1) + ((dstY & 1u) == 0u ? -1 : 0);
    float sum = 0.0;
    float wsum = 0.0;

    [unroll]
    for (int j = -2; j <= 3; ++j) {
        int sy = clamp(baseY + j, 0, h - 1);
        float w = wyTap(dstY & 1u, j);
        sum += elY.Load(int3(sx, sy, 0)) * w;
        wsum += w;
    }

    float v = abs(wsum) > 1e-8 ? sum / wsum : elY.Load(int3(sx, clamp(baseY, 0, h - 1), 0));
    return fp16Round(v);
}

float2 verticalElUvNorm(int sx, uint dstY)
{
    int h = int(max(elHeight >> 1, 1));
    int baseY = int(dstY >> 1) + ((dstY & 1u) == 0u ? -1 : 0);
    float2 sum = float2(0.0, 0.0);
    float wsum = 0.0;

    [unroll]
    for (int j = -2; j <= 3; ++j) {
        int sy = clamp(baseY + j, 0, h - 1);
        float w = wyTap(dstY & 1u, j);
        sum += elUV.Load(int3(sx, sy, 0)) * w;
        wsum += w;
    }

    float2 v = abs(wsum) > 1e-8 ? sum / wsum : elUV.Load(int3(sx, clamp(baseY, 0, h - 1), 0));
    return fp16Round2(v);
}

float sampleElYNorm(uint2 dst)
{
    int w = int(elWidth);
    int baseX = int(dst.x >> 1);
    float sum = 0.0;
    float wsum = 0.0;

    [unroll]
    for (int i = -2; i <= 3; ++i) {
        int sx = clamp(baseX + i, 0, w - 1);
        float wx = wxTap(dst.x & 1u, i);
        sum += verticalElYNorm(sx, dst.y) * wx;
        wsum += wx;
    }

    return abs(wsum) > 1e-8 ? sum / wsum : verticalElYNorm(clamp(baseX, 0, w - 1), dst.y);
}

float2 sampleElUvNorm(uint2 dst)
{
    int w = int(max(elWidth >> 1, 1));
    int baseX = int(dst.x >> 1);
    float2 sum = float2(0.0, 0.0);
    float wsum = 0.0;

    [unroll]
    for (int i = -2; i <= 3; ++i) {
        int sx = clamp(baseX + i, 0, w - 1);
        float wx = wxTap(dst.x & 1u, i);
        sum += verticalElUvNorm(sx, dst.y) * wx;
        wsum += wx;
    }

    return abs(wsum) > 1e-8 ? sum / wsum : verticalElUvNorm(clamp(baseX, 0, w - 1), dst.y);
}

float composeNlqNorm(float blNorm, float elNorm, float oCode, float sCode, float tCode)
{
    float o = oCode / 1023.0;
    float s = sCode * 1023.0;
    float t = tCode - 0.5 * sCode;
    float c = elNorm - o;

    if (c == 0.0)
        return saturate(blNorm);

    float residual = sign(c) * (abs(c) * s + t);
    return saturate(blNorm + residual);
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x < blWidth && tid.y < blHeight) {
        float blNorm = blY.Load(int3(tid.xy, 0));
        float elNorm = sampleElYNorm(tid.xy);
        float outNorm = composeNlqNorm(blNorm, elNorm, offsetCode.x, slope.x, threshold.x);

        if (carrierMode == 0) {
            float code = round(outNorm * (65535.0 / 64.0));
            outNorm = (clamp(code, 0.0, 1023.0) * 64.0) / 65535.0;
        }

        float finalY = (splitMode != 0u && tid.x < blWidth / 2u) ? blNorm : outNorm;
        if (splitMode == 2u && tid.x >= blWidth / 2u)
            finalY = saturate(0.0625 + abs(outNorm - blNorm) * 48.0);
        outY[tid.xy] = finalY;
    }

    uint uvWidth = blWidth >> 1;
    uint uvHeight = blHeight >> 1;
    if (tid.x < uvWidth && tid.y < uvHeight) {
        float2 blNorm = blUV.Load(int3(tid.xy, 0));
        float2 elNorm = sampleElUvNorm(tid.xy);
        float2 outNorm;
        outNorm.x = composeNlqNorm(blNorm.x, elNorm.x, offsetCode.y, slope.y, threshold.y);
        outNorm.y = composeNlqNorm(blNorm.y, elNorm.y, offsetCode.z, slope.z, threshold.z);

        if (carrierMode == 0) {
            float2 code = round(outNorm * (65535.0 / 64.0));
            outNorm = (clamp(code, 0.0, 1023.0) * 64.0) / 65535.0;
        }

        float2 finalUV = (splitMode != 0u && tid.x < uvWidth / 2u) ? blNorm : outNorm;
        if (splitMode == 2u && tid.x >= uvWidth / 2u)
            finalUV = float2(0.5, 0.5);
        outUV[tid.xy] = finalUV;
    }
}
)HLSL";

static const char* kFelP016ComputeShader = R"HLSL(
cbuffer FelConstants : register(b0)
{
    uint blWidth;
    uint blHeight;
    uint elWidth;
    uint elHeight;
    uint carrierMode;
    uint splitMode;
    uint2 carrierPad;
    float4 offsetCode;
    float4 slope;
    float4 threshold;
};

Texture2D<float>  blY  : register(t0);
Texture2D<float2> blUV : register(t1);
Texture2D<float>  elY  : register(t2);
Texture2D<float2> elUV : register(t3);
RWTexture2D<float>  outY  : register(u0);
RWTexture2D<float2> outUV : register(u1);

float codeFromP010Norm(float v) { return round(v * (65535.0 / 64.0)); }
float2 codeFromP010Norm2(float2 v) { return round(v * (65535.0 / 64.0)); }

float felComposeNorm(float blCode, float elCode, float o, float s, float t)
{
    float rr = elCode - o;
    float base = blCode / 1023.0;
    if (abs(rr) < 0.00001) return saturate(base);
    float residual = ((abs(rr) - 0.5) * s + t) * (rr < 0.0 ? -1.0 : 1.0);
    return saturate(base + residual);
}

float wxTap(uint parity, int tap)
{
    if (parity == 0) {
        if (tap == -2) return 0.0;
        if (tap == -1) return 0.0;
        if (tap ==  0) return 1.0;
        if (tap ==  1) return 0.0;
        if (tap ==  2) return 0.0;
        return 0.0;
    }

    if (tap == -2) return  0.0244598389;
    if (tap == -1) return -0.1358642578;
    if (tap ==  0) return  0.6115722656;
    if (tap ==  1) return  0.6115722656;
    if (tap ==  2) return -0.1358642578;
    return 0.0244598389;
}

float wyTap(uint parity, int tap)
{
    if (parity == 0) {
        if (tap == -2) return  0.0073785782;
        if (tap == -1) return -0.0679931641;
        if (tap ==  0) return  0.2709350586;
        if (tap ==  1) return  0.8928222656;
        if (tap ==  2) return -0.1333007813;
        return 0.0301132202;
    }

    if (tap == -2) return  0.0301132202;
    if (tap == -1) return -0.1333007813;
    if (tap ==  0) return  0.8928222656;
    if (tap ==  1) return  0.2709350586;
    if (tap ==  2) return -0.0679931641;
    return 0.0073785782;
}

float sampleElYCode(uint2 dst)
{
    int2 size = int2(elWidth, elHeight);
    float2 pos = float2(float(dst.x) * 0.5, float(dst.y) * 0.5 - 0.25);
    int2 base = int2(floor(pos));
    float sum = 0.0;
    float wsum = 0.0;

    [unroll]
    for (int j = -2; j <= 3; ++j) {
        int sy = clamp(base.y + j, 0, size.y - 1);
        float wy = wyTap(dst.y & 1u, j);
        [unroll]
        for (int i = -2; i <= 3; ++i) {
            int sx = clamp(base.x + i, 0, size.x - 1);
            float wx = wxTap(dst.x & 1u, i);
            float w = wx * wy;
            sum += codeFromP010Norm(elY.Load(int3(sx, sy, 0))) * w;
            wsum += w;
        }
    }

    return abs(wsum) > 1e-8 ? sum / wsum : codeFromP010Norm(elY.Load(int3(clamp(base, int2(0,0), size - 1), 0)));
}

float2 sampleElUvCode(uint2 dst)
{
    int2 size = int2(max(elWidth >> 1, 1), max(elHeight >> 1, 1));
    float2 pos = float2(float(dst.x) * 0.5, float(dst.y) * 0.5 - 0.25);
    int2 base = int2(floor(pos));
    float2 sum = float2(0.0, 0.0);
    float wsum = 0.0;

    [unroll]
    for (int j = -2; j <= 3; ++j) {
        int sy = clamp(base.y + j, 0, size.y - 1);
        float wy = wyTap(dst.y & 1u, j);
        [unroll]
        for (int i = -2; i <= 3; ++i) {
            int sx = clamp(base.x + i, 0, size.x - 1);
            float wx = wxTap(dst.x & 1u, i);
            float w = wx * wy;
            sum += codeFromP010Norm2(elUV.Load(int3(sx, sy, 0))) * w;
            wsum += w;
        }
    }

    return abs(wsum) > 1e-8 ? sum / wsum : codeFromP010Norm2(elUV.Load(int3(clamp(base, int2(0,0), size - 1), 0)));
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x < blWidth && tid.y < blHeight) {
        float blCode = codeFromP010Norm(blY.Load(int3(tid.xy, 0)));
        float elCode = sampleElYCode(tid.xy);
        float blNorm = blY.Load(int3(tid.xy, 0));
        float felNorm = felComposeNorm(blCode, elCode, offsetCode.x, slope.x, threshold.x);
        float finalY = (splitMode != 0u && tid.x < blWidth / 2u) ? blNorm : felNorm;
        if (splitMode == 2u && tid.x >= blWidth / 2u)
            finalY = saturate(0.0625 + abs(felNorm - blNorm) * 48.0);
        outY[tid.xy] = finalY;
    }

    uint uvWidth = blWidth >> 1;
    uint uvHeight = blHeight >> 1;
    if (tid.x < uvWidth && tid.y < uvHeight) {
        float2 blCode = codeFromP010Norm2(blUV.Load(int3(tid.xy, 0)));
        float2 elCode = sampleElUvCode(tid.xy);
        float2 blNorm = blUV.Load(int3(tid.xy, 0));
        float2 felNorm = float2(
            felComposeNorm(blCode.x, elCode.x, offsetCode.y, slope.y, threshold.y),
            felComposeNorm(blCode.y, elCode.y, offsetCode.z, slope.z, threshold.z));
        float2 finalUV = (splitMode != 0u && tid.x < uvWidth / 2u) ? blNorm : felNorm;
        if (splitMode == 2u && tid.x >= uvWidth / 2u)
            finalUV = float2(0.5, 0.5);
        outUV[tid.xy] = finalUV;
    }
}
)HLSL";

static bool CreatePlaneSrv(
    ID3D11Device3* device3,
    ID3D11Texture2D* texture,
    DXGI_FORMAT format,
    UINT plane,
    ID3D11ShaderResourceView1** outView) {

    if (!device3 || !texture || !outView) return false;
    *outView = nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC1 desc{};
    desc.Format = format;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MostDetailedMip = 0;
    desc.Texture2D.MipLevels = 1;
    desc.Texture2D.PlaneSlice = plane;

    return SUCCEEDED(device3->CreateShaderResourceView1(texture, &desc, outView)) && *outView;
}

static bool CreatePlaneUav(
    ID3D11Device3* device3,
    ID3D11Texture2D* texture,
    DXGI_FORMAT format,
    UINT plane,
    ID3D11UnorderedAccessView1** outView) {

    if (!device3 || !texture || !outView) return false;
    *outView = nullptr;

    D3D11_UNORDERED_ACCESS_VIEW_DESC1 desc{};
    desc.Format = format;
    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.PlaneSlice = plane;

    return SUCCEEDED(device3->CreateUnorderedAccessView1(texture, &desc, outView)) && *outView;
}

static bool EnsureGpuFelState(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC& blDesc,
    UINT elWidth,
    UINT elHeight) {

    if (!device || blDesc.Format != DXGI_FORMAT_P010 || !elWidth || !elHeight) return false;

    const bool same =
        g_gpuFelDevice == device &&
        g_gpuBlInput &&
        g_gpuBlOutput &&
        g_gpuElInput &&
        g_gpuFelShader &&
        g_gpuFelP016Shader &&
        g_gpuFelConstants &&
        g_gpuFelBlWidth == blDesc.Width &&
        g_gpuFelBlHeight == blDesc.Height &&
        g_gpuFelElWidth == elWidth &&
        g_gpuFelElHeight == elHeight &&
        g_gpuFelFormat == blDesc.Format;

    if (same) return true;

    ReleaseGpuFelState();

    ID3D11Device3* device3 = nullptr;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device3));
    if (FAILED(hr) || !device3) {
        Log(L"FEL GPU INIT FAIL reason=ID3D11Device3 hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC blGpu{};
    blGpu.Width = blDesc.Width;
    blGpu.Height = blDesc.Height;
    blGpu.MipLevels = 1;
    blGpu.ArraySize = 1;
    blGpu.Format = DXGI_FORMAT_P010;
    blGpu.SampleDesc.Count = 1;
    blGpu.Usage = D3D11_USAGE_DEFAULT;
    blGpu.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = device->CreateTexture2D(&blGpu, nullptr, &g_gpuBlInput);
    if (FAILED(hr)) {
        Log(L"FEL GPU INIT FAIL reason=BL_INPUT_TEXTURE hr=0x%08X", static_cast<unsigned>(hr));
        device3->Release();
        return false;
    }

    blGpu.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr = device->CreateTexture2D(&blGpu, nullptr, &g_gpuBlOutput);
    if (FAILED(hr)) {
        Log(L"FEL GPU INIT FAIL reason=P010_UAV_TEXTURE hr=0x%08X", static_cast<unsigned>(hr));
        device3->Release();
        ReleaseGpuFelState();
        return false;
    }

    D3D11_TEXTURE2D_DESC elDesc{};
    elDesc.Width = elWidth;
    elDesc.Height = elHeight;
    elDesc.MipLevels = 1;
    elDesc.ArraySize = 1;
    elDesc.Format = DXGI_FORMAT_P010;
    elDesc.SampleDesc.Count = 1;
    elDesc.Usage = D3D11_USAGE_DEFAULT;
    elDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = device->CreateTexture2D(&elDesc, nullptr, &g_gpuElInput);
    if (FAILED(hr)) {
        Log(L"FEL GPU INIT FAIL reason=EL_TEXTURE hr=0x%08X", static_cast<unsigned>(hr));
        device3->Release();
        ReleaseGpuFelState();
        return false;
    }

    if (!CreatePlaneSrv(device3, g_gpuBlInput, DXGI_FORMAT_R16_UNORM, 0, &g_gpuBlYSrv) ||
        !CreatePlaneSrv(device3, g_gpuBlInput, DXGI_FORMAT_R16G16_UNORM, 1, &g_gpuBlUvSrv) ||
        !CreatePlaneSrv(device3, g_gpuElInput, DXGI_FORMAT_R16_UNORM, 0, &g_gpuElYSrv) ||
        !CreatePlaneSrv(device3, g_gpuElInput, DXGI_FORMAT_R16G16_UNORM, 1, &g_gpuElUvSrv) ||
        !CreatePlaneUav(device3, g_gpuBlOutput, DXGI_FORMAT_R16_UNORM, 0, &g_gpuOutYUav) ||
        !CreatePlaneUav(device3, g_gpuBlOutput, DXGI_FORMAT_R16G16_UNORM, 1, &g_gpuOutUvUav)) {

        Log(L"FEL GPU INIT FAIL reason=P010_PLANE_VIEWS");
        device3->Release();
        ReleaseGpuFelState();
        return false;
    }

    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errors = nullptr;
    hr = D3DCompile(
        kFelComputeShader,
        strlen(kFelComputeShader),
        "P7FEL",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errors);

    if (FAILED(hr) || !shaderBlob) {
        if (errors && errors->GetBufferPointer()) {
            Log(L"FEL GPU SHADER COMPILE FAIL hr=0x%08X bytes=%u",
                static_cast<unsigned>(hr),
                static_cast<unsigned>(errors->GetBufferSize()));
        } else {
            Log(L"FEL GPU SHADER COMPILE FAIL hr=0x%08X", static_cast<unsigned>(hr));
        }
        if (errors) errors->Release();
        if (shaderBlob) shaderBlob->Release();
        device3->Release();
        ReleaseGpuFelState();
        return false;
    }
    if (errors) errors->Release();

    Log(L"FEL POST-RGB INIT compile done bytes=%u", static_cast<unsigned>(shaderBlob->GetBufferSize()));

    hr = device->CreateComputeShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        nullptr,
        &g_gpuFelShader);
    shaderBlob->Release();

    if (FAILED(hr) || !g_gpuFelShader) {
        Log(L"FEL GPU INIT FAIL reason=COMPUTE_SHADER hr=0x%08X", static_cast<unsigned>(hr));
        device3->Release();
        ReleaseGpuFelState();
        return false;
    }

    shaderBlob = nullptr;
    errors = nullptr;
    hr = D3DCompile(
        kFelP016ComputeShader,
        strlen(kFelP016ComputeShader),
        "P7FEL-P016",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errors);
    if (FAILED(hr) || !shaderBlob) {
        Log(L"FEL P016 SHADER COMPILE FAIL hr=0x%08X", static_cast<unsigned>(hr));
        if (errors) errors->Release();
        if (shaderBlob) shaderBlob->Release();
        device3->Release();
        ReleaseGpuFelState();
        return false;
    }
    if (errors) errors->Release();
    hr = device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &g_gpuFelP016Shader);
    shaderBlob->Release();
    if (FAILED(hr) || !g_gpuFelP016Shader) {
        Log(L"FEL GPU INIT FAIL reason=P016_COMPUTE_SHADER hr=0x%08X", static_cast<unsigned>(hr));
        device3->Release();
        ReleaseGpuFelState();
        return false;
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = static_cast<UINT>((sizeof(GpuFelConstants) + 15u) & ~15u);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    hr = device->CreateBuffer(&cb, nullptr, &g_gpuFelConstants);
    if (FAILED(hr) || !g_gpuFelConstants) {
        Log(L"FEL GPU INIT FAIL reason=CONSTANT_BUFFER hr=0x%08X", static_cast<unsigned>(hr));
        device3->Release();
        ReleaseGpuFelState();
        return false;
    }

    device->AddRef();
    g_gpuFelDevice = device;
    g_gpuFelDevice3 = device3;
    g_gpuFelBlWidth = blDesc.Width;
    g_gpuFelBlHeight = blDesc.Height;
    g_gpuFelElWidth = elWidth;
    g_gpuFelElHeight = elHeight;
    g_gpuFelFormat = blDesc.Format;
    InterlockedExchange(&g_gpuRendererReady, 1);

    Log(L"FEL GPU READY api=D3D11_COMPUTE cs=5_0 BL=%ux%u EL=%ux%u format=P010 sample=LIBPLACEBO_EXACT_LUT16_NATIVE_P010",
        blDesc.Width, blDesc.Height, elWidth, elHeight);
    return true;
}



static const char* kPostDolbyRgbFelShader = R"HLSL(
cbuffer PostDolbyRgbConstants : register(b0)
{
    uint width;
    uint height;
    uint elWidth;
    uint elHeight;
    uint splitMode;
    uint lumaGuardMode;
    uint hdr10Available;
    uint paddingSplit;
    float4 offsetCode;
    float4 slope;
    float4 threshold;
    float4 nonlinear0;
    float4 nonlinear1;
    float4 nonlinear2;
    float4 post0;
    float4 post1;
    float4 post2;
    float4 invPost0;
    float4 invPost1;
    float4 invPost2;
};

Texture2D<float4> rgbIn : register(t0);
Texture2D<float> elY : register(t1);
Texture2D<float2> elUV : register(t2);
Texture2D<float> hdr10Y : register(t3);
Texture2D<float2> hdr10UV : register(t4);
RWTexture2D<float4> rgbOut : register(u0);
RWByteAddressBuffer clipStats : register(u1);

groupshared uint gClipPixels;
groupshared uint gLowPixels;
groupshared uint gHighPixels;
groupshared uint gGuardPixels;

float codeFromP010Norm(float v)
{
    return round(v * (65535.0 / 64.0));
}

float2 codeFromP010Norm2(float2 v)
{
    return round(v * (65535.0 / 64.0));
}

float wxTap(uint parity, int tap)
{
    if (parity == 0) {
        if (tap == -2) return 0.0;
        if (tap == -1) return 0.0;
        if (tap ==  0) return 1.0;
        if (tap ==  1) return 0.0;
        if (tap ==  2) return 0.0;
        return 0.0;
    }

    if (tap == -2) return  0.0244598389;
    if (tap == -1) return -0.1358642578;
    if (tap ==  0) return  0.6115722656;
    if (tap ==  1) return  0.6115722656;
    if (tap ==  2) return -0.1358642578;
    return 0.0244598389;
}

float wyTap(uint parity, int tap)
{
    if (parity == 0) {
        if (tap == -2) return  0.0073785782;
        if (tap == -1) return -0.0679931641;
        if (tap ==  0) return  0.2709350586;
        if (tap ==  1) return  0.8928222656;
        if (tap ==  2) return -0.1333007813;
        return 0.0301132202;
    }

    if (tap == -2) return  0.0301132202;
    if (tap == -1) return -0.1333007813;
    if (tap ==  0) return  0.8928222656;
    if (tap ==  1) return  0.2709350586;
    if (tap ==  2) return -0.0679931641;
    return 0.0073785782;
}

float sampleElYCode(uint2 dst)
{
    int2 size = int2(elWidth, elHeight);
    float2 pos = float2(float(dst.x) * 0.5, float(dst.y) * 0.5 - 0.25);
    int2 base = int2(floor(pos));
    float sum = 0.0;
    float wsum = 0.0;

    [unroll]
    for (int j = -2; j <= 3; ++j) {
        int sy = clamp(base.y + j, 0, size.y - 1);
        float wy = wyTap(dst.y & 1u, j);

        [unroll]
        for (int i = -2; i <= 3; ++i) {
            int sx = clamp(base.x + i, 0, size.x - 1);
            float wx = wxTap(dst.x & 1u, i);
            float w = wx * wy;
            sum += codeFromP010Norm(elY.Load(int3(sx, sy, 0))) * w;
            wsum += w;
        }
    }

    return abs(wsum) > 1e-8
        ? sum / wsum
        : codeFromP010Norm(elY.Load(int3(clamp(base, int2(0, 0), size - 1), 0)));
}

float2 sampleElUvAtBlChromaCode(uint2 dst)
{
    int2 size = int2(max(elWidth >> 1, 1), max(elHeight >> 1, 1));
    float2 pos = float2(float(dst.x) * 0.5, float(dst.y) * 0.5 - 0.25);
    int2 base = int2(floor(pos));
    float2 sum = float2(0.0, 0.0);
    float wsum = 0.0;

    [unroll]
    for (int j = -2; j <= 3; ++j) {
        int sy = clamp(base.y + j, 0, size.y - 1);
        float wy = wyTap(dst.y & 1u, j);

        [unroll]
        for (int i = -2; i <= 3; ++i) {
            int sx = clamp(base.x + i, 0, size.x - 1);
            float wx = wxTap(dst.x & 1u, i);
            float w = wx * wy;
            sum += codeFromP010Norm2(elUV.Load(int3(sx, sy, 0))) * w;
            wsum += w;
        }
    }

    return abs(wsum) > 1e-8
        ? sum / wsum
        : codeFromP010Norm2(elUV.Load(int3(clamp(base, int2(0, 0), size - 1), 0)));
}

float2 sampleElUvFullCode(uint2 dst)
{
    int2 size = int2(max(int(elWidth >> 1), 1), max(int(elHeight >> 1), 1));
    float2 pos = float2(float(dst.x) * 0.25, float(dst.y) * 0.25 - 0.375);
    int2 base = int2(floor(pos));
    float2 sum = float2(0.0, 0.0);
    float wsum = 0.0;

    uint phaseX = dst.x & 3u;
    uint phaseY = dst.y & 3u;
    float fracX = frac(pos.x);
    float fracY = frac(pos.y);

    [unroll]
    for (int j = -2; j <= 3; ++j) {
        int sy = clamp(base.y + j, 0, size.y - 1);
        float dy = float(j) - fracY;
        float ay = abs(dy);
        float wy = ay < 1e-6 ? 1.0 : (ay < 3.0 ? (sin(3.14159265359 * dy) / (3.14159265359 * dy)) * (sin(3.14159265359 * dy / 3.0) / (3.14159265359 * dy / 3.0)) : 0.0);

        [unroll]
        for (int i = -2; i <= 3; ++i) {
            int sx = clamp(base.x + i, 0, size.x - 1);
            float dx = float(i) - fracX;
            float ax = abs(dx);
            float wx = ax < 1e-6 ? 1.0 : (ax < 3.0 ? (sin(3.14159265359 * dx) / (3.14159265359 * dx)) * (sin(3.14159265359 * dx / 3.0) / (3.14159265359 * dx / 3.0)) : 0.0);
            float w = wx * wy;
            sum += codeFromP010Norm2(elUV.Load(int3(sx, sy, 0))) * w;
            wsum += w;
        }
    }

    return abs(wsum) > 1e-8
        ? sum / wsum
        : codeFromP010Norm2(elUV.Load(int3(clamp(base, int2(0,0), size - 1), 0)));
}

float nlqResidual(float elCode, float o, float s, float t)
{
    float rr = elCode - o;

    if (abs(rr) < 0.00001)
        return 0.0;

    return ((abs(rr) - 0.5) * s + t) * (rr < 0.0 ? -1.0 : 1.0);
}

float3 pqEotf(float3 x)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    x = pow(max(x, 0.0), 1.0 / m2);
    x = max(x - c1, 0.0) / max(c2 - c3 * x, 1e-8);
    return pow(x, 1.0 / m1);
}

float3 pqOetf(float3 x)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    x = pow(max(x, 0.0), m1);
    x = (c1 + c2 * x) / (1.0 + c3 * x);
    return pow(max(x, 0.0), m2);
}

float3 mulPost(float3 v)
{
    return float3(dot(post0.xyz, v), dot(post1.xyz, v), dot(post2.xyz, v));
}

float3 mulInvPost(float3 v)
{
    return float3(dot(invPost0.xyz, v), dot(invPost1.xyz, v), dot(invPost2.xyz, v));
}

[numthreads(16, 16, 1)]
void main(
    uint3 tid : SV_DispatchThreadID,
    uint3 groupTid : SV_GroupThreadID)
{
    const uint localIndex = groupTid.y * 16u + groupTid.x;

    if (localIndex == 0u) {
        gClipPixels = 0u;
        gLowPixels = 0u;
        gHighPixels = 0u;
        gGuardPixels = 0u;
    }

    GroupMemoryBarrierWithGroupSync();

    const bool inBounds = tid.x < width && tid.y < height;

    if (inBounds) {
        float y = sampleElYCode(tid.xy);
    float2 uv = sampleElUvFullCode(tid.xy);

    float3 residual;
    residual.x = nlqResidual(y, offsetCode.x, slope.x, threshold.x);
    residual.y = nlqResidual(uv.x, offsetCode.y, slope.y, threshold.y);
    residual.z = nlqResidual(uv.y, offsetCode.z, slope.z, threshold.z);

    float3 deltaNonlinear = float3(
        dot(nonlinear0.xyz, residual),
        dot(nonlinear1.xyz, residual),
        dot(nonlinear2.xyz, residual));

        float4 src = rgbIn.Load(int3(tid.xy, 0));
        float3 baseLinearRgb = pqEotf(src.rgb);
        float3 baseLinearLms = mulInvPost(baseLinearRgb);
        float3 baseNonlinear = pqOetf(baseLinearLms);
        float3 reconstructedNonlinear = baseNonlinear + deltaNonlinear;
        float3 reconstructedLinearLms = pqEotf(reconstructedNonlinear);
        float3 reconstructedLinearRgb = mulPost(reconstructedLinearLms);
        float3 rawRgb = pqOetf(reconstructedLinearRgb);

        const bool low = any(rawRgb < 0.0);
        const bool high = any(rawRgb > 1.0);

        uint ignored = 0u;
        if (low || high)
            InterlockedAdd(gClipPixels, 1u, ignored);
        if (low)
            InterlockedAdd(gLowPixels, 1u, ignored);
        if (high)
            InterlockedAdd(gHighPixels, 1u, ignored);

        float3 outputRgb = saturate(rawRgb);
        if (false && lumaGuardMode != 0u && splitMode == 0u) {
            const float3 weights = float3(0.2627, 0.6780, 0.0593);
            const float3 dolbyLinear = pqEotf(saturate(src.rgb));
            const float3 felLinear = pqEotf(outputRgb);
            const float dolbyLuma = dot(dolbyLinear, weights);
            const float felLuma = dot(felLinear, weights);
            if (felLuma > dolbyLuma) {
                const float gain = dolbyLuma / max(felLuma, 1e-10);
                outputRgb = saturate(pqOetf(felLinear * gain));
                if (splitMode == 0u || tid.x >= width / 2u) {
                    uint ignoredGuard = 0u;
                    InterlockedAdd(gGuardPixels, 1u, ignoredGuard);
                }
            }
        }
        float3 finalRgb = outputRgb;
        if (splitMode != 0u && tid.x < width / 2u) {
            if (hdr10Available != 0u) {
                float yc = round(hdr10Y.Load(int3(tid.xy, 0)) * (65535.0 / 64.0));
                float2 cc = round(hdr10UV.Load(int3(tid.xy / 2u, 0)) * (65535.0 / 64.0));
                float yv = (yc - 64.0) / 876.0;
                float cb = (cc.x - 512.0) / 896.0;
                float cr = (cc.y - 512.0) / 896.0;
                float3 bt2020 = float3(
                    yv + 1.4746 * cr,
                    yv - 0.1645531268 * cb - 0.5713531268 * cr,
                    yv + 1.8814 * cb);
                finalRgb = saturate(bt2020);
            } else {
                finalRgb = src.rgb;
            }
        }
        if (splitMode == 2u && tid.x >= width / 2u) {
            float3 diff = abs(outputRgb - src.rgb);
            float intensity = saturate(max(diff.r, max(diff.g, diff.b)) * 48.0);
            finalRgb = float3(intensity, 0.0, intensity);
        }
        rgbOut[tid.xy] = float4(finalRgb, src.a);
    }

    GroupMemoryBarrierWithGroupSync();

    if (localIndex == 0u) {
        uint ignored = 0u;
        clipStats.InterlockedAdd(0u, gClipPixels, ignored);
        clipStats.InterlockedAdd(4u, gLowPixels, ignored);
        clipStats.InterlockedAdd(8u, gHighPixels, ignored);
        clipStats.InterlockedAdd(12u, gGuardPixels, ignored);
    }
}
)HLSL";

static void ReleasePostDolbyRgbState() {
    if (g_postRgbClipStatsReadback) { g_postRgbClipStatsReadback->Release(); g_postRgbClipStatsReadback = nullptr; }
    if (g_postRgbClipStatsUav) { g_postRgbClipStatsUav->Release(); g_postRgbClipStatsUav = nullptr; }
    if (g_postRgbClipStats) { g_postRgbClipStats->Release(); g_postRgbClipStats = nullptr; }
    g_postRgbClipWindowFrames = 0;
    g_postRgbClipWindowsLogged = 0;

    if (g_postRgbConstants) { g_postRgbConstants->Release(); g_postRgbConstants = nullptr; }
    if (g_postRgbShader) { g_postRgbShader->Release(); g_postRgbShader = nullptr; }
    if (g_postRgbOutputUav) { g_postRgbOutputUav->Release(); g_postRgbOutputUav = nullptr; }
    if (g_postRgbInputSrv) { g_postRgbInputSrv->Release(); g_postRgbInputSrv = nullptr; }
    if (g_postRgbElYSrv) { g_postRgbElYSrv->Release(); g_postRgbElYSrv = nullptr; }
    if (g_postRgbHdr10YSrv) { g_postRgbHdr10YSrv->Release(); g_postRgbHdr10YSrv = nullptr; }
    if (g_postRgbHdr10UvSrv) { g_postRgbHdr10UvSrv->Release(); g_postRgbHdr10UvSrv = nullptr; }
    if (g_postRgbHdr10Input) { g_postRgbHdr10Input->Release(); g_postRgbHdr10Input = nullptr; }
    if (g_postRgbElUvSrv) { g_postRgbElUvSrv->Release(); g_postRgbElUvSrv = nullptr; }
    if (g_postRgbInput) { g_postRgbInput->Release(); g_postRgbInput = nullptr; }
    if (g_postRgbOutput) { g_postRgbOutput->Release(); g_postRgbOutput = nullptr; }
    if (g_postRgbElInput) { g_postRgbElInput->Release(); g_postRgbElInput = nullptr; }
    if (g_postRgbDevice3) { g_postRgbDevice3->Release(); g_postRgbDevice3 = nullptr; }
    if (g_postRgbDevice) { g_postRgbDevice->Release(); g_postRgbDevice = nullptr; }

    g_postRgbWidth = 0;
    g_postRgbHeight = 0;
    g_postRgbElWidth = 0;
    g_postRgbElHeight = 0;
}

static bool EnsurePostDolbyRgbState(
    ID3D11Device* device,
    UINT width,
    UINT height,
    UINT elWidth,
    UINT elHeight) {

    if (!device || !width || !height || !elWidth || !elHeight) return false;

    const bool same =
        g_postRgbDevice == device &&
        g_postRgbInput &&
        g_postRgbOutput &&
        g_postRgbElInput &&
        g_postRgbHdr10Input && g_postRgbHdr10YSrv && g_postRgbHdr10UvSrv &&
        g_postRgbInputSrv &&
        g_postRgbOutputUav &&
        g_postRgbElYSrv &&
        g_postRgbElUvSrv &&
        g_postRgbShader &&
        g_postRgbConstants &&
        g_postRgbClipStats &&
        g_postRgbClipStatsUav &&
        g_postRgbClipStatsReadback &&
        g_postRgbWidth == width &&
        g_postRgbHeight == height &&
        g_postRgbElWidth == elWidth &&
        g_postRgbElHeight == elHeight;

    if (same) return true;

    ReleasePostDolbyRgbState();

    ID3D11Device3* device3 = nullptr;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device3));
    if (FAILED(hr) || !device3) {
        Log(L"FEL POST-RGB INIT FAIL reason=ID3D11Device3 hr=0x%08X",
            static_cast<unsigned>(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC rgbDesc{};
    rgbDesc.Width = width;
    rgbDesc.Height = height;
    rgbDesc.MipLevels = 1;
    rgbDesc.ArraySize = 1;
    rgbDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    rgbDesc.SampleDesc.Count = 1;
    rgbDesc.Usage = D3D11_USAGE_DEFAULT;
    rgbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = device->CreateTexture2D(&rgbDesc, nullptr, &g_postRgbInput);
    if (FAILED(hr) || !g_postRgbInput) {
        Log(L"FEL POST-RGB INIT FAIL reason=RGB_INPUT hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        return false;
    }

    rgbDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr = device->CreateTexture2D(&rgbDesc, nullptr, &g_postRgbOutput);
    if (FAILED(hr) || !g_postRgbOutput) {
        Log(L"FEL POST-RGB INIT FAIL reason=RGB_OUTPUT_UAV hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC rgbSrvDesc{};
    rgbSrvDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    rgbSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    rgbSrvDesc.Texture2D.MostDetailedMip = 0;
    rgbSrvDesc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(
        g_postRgbInput,
        &rgbSrvDesc,
        &g_postRgbInputSrv);

    if (FAILED(hr) || !g_postRgbInputSrv) {
        Log(L"FEL POST-RGB INIT FAIL reason=RGB_SRV hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC rgbUavDesc{};
    rgbUavDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    rgbUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    rgbUavDesc.Texture2D.MipSlice = 0;

    hr = device->CreateUnorderedAccessView(
        g_postRgbOutput,
        &rgbUavDesc,
        &g_postRgbOutputUav);

    if (FAILED(hr) || !g_postRgbOutputUav) {
        Log(L"FEL POST-RGB INIT FAIL reason=RGB_UAV hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    D3D11_TEXTURE2D_DESC elDesc{};
    elDesc.Width = elWidth;
    elDesc.Height = elHeight;
    elDesc.MipLevels = 1;
    elDesc.ArraySize = 1;
    elDesc.Format = DXGI_FORMAT_P010;
    elDesc.SampleDesc.Count = 1;
    elDesc.Usage = D3D11_USAGE_DEFAULT;
    elDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = device->CreateTexture2D(&elDesc, nullptr, &g_postRgbElInput);
    if (FAILED(hr) || !g_postRgbElInput) {
        Log(L"FEL POST-RGB INIT FAIL reason=EL_INPUT hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    D3D11_TEXTURE2D_DESC hdrDesc = elDesc;
    hdrDesc.Width = width;
    hdrDesc.Height = height;
    hr = device->CreateTexture2D(&hdrDesc, nullptr, &g_postRgbHdr10Input);
    if (FAILED(hr) || !g_postRgbHdr10Input ||
        !CreatePlaneSrv(device3, g_postRgbHdr10Input, DXGI_FORMAT_R16_UNORM, 0, &g_postRgbHdr10YSrv) ||
        !CreatePlaneSrv(device3, g_postRgbHdr10Input, DXGI_FORMAT_R16G16_UNORM, 1, &g_postRgbHdr10UvSrv)) {
        Log(L"FEL HDR10 BL INIT FAIL hr=0x%08X", static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    if (!CreatePlaneSrv(device3, g_postRgbElInput, DXGI_FORMAT_R16_UNORM, 0, &g_postRgbElYSrv) ||
        !CreatePlaneSrv(device3, g_postRgbElInput, DXGI_FORMAT_R16G16_UNORM, 1, &g_postRgbElUvSrv)) {

        Log(L"FEL POST-RGB INIT FAIL reason=EL_PLANE_SRVS");
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errors = nullptr;
    Log(L"FEL POST-RGB INIT compile begin size=%ux%u EL=%ux%u", width, height, elWidth, elHeight);
    hr = D3DCompile(
        kPostDolbyRgbFelShader,
        strlen(kPostDolbyRgbFelShader),
        "P7FEL-POST-RGB",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shaderBlob,
        &errors);

    if (FAILED(hr) || !shaderBlob) {
        Log(L"FEL POST-RGB SHADER COMPILE FAIL hr=0x%08X",
            static_cast<unsigned>(hr));
        if (errors) errors->Release();
        if (shaderBlob) shaderBlob->Release();
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }
    if (errors) errors->Release();

    hr = device->CreateComputeShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        nullptr,
        &g_postRgbShader);
    shaderBlob->Release();

    if (FAILED(hr) || !g_postRgbShader) {
        Log(L"FEL POST-RGB INIT FAIL reason=COMPUTE_SHADER hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = static_cast<UINT>((sizeof(PostDolbyRgbConstants) + 15u) & ~15u);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    hr = device->CreateBuffer(&cb, nullptr, &g_postRgbConstants);
    if (FAILED(hr) || !g_postRgbConstants) {
        Log(L"FEL POST-RGB INIT FAIL reason=CONSTANT_BUFFER hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    UINT clipZeros[4] = {0, 0, 0, 0};
    D3D11_BUFFER_DESC clipDesc{};
    clipDesc.ByteWidth = sizeof(clipZeros);
    clipDesc.Usage = D3D11_USAGE_DEFAULT;
    clipDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    clipDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

    D3D11_SUBRESOURCE_DATA clipInit{};
    clipInit.pSysMem = clipZeros;

    hr = device->CreateBuffer(&clipDesc, &clipInit, &g_postRgbClipStats);
    if (FAILED(hr) || !g_postRgbClipStats) {
        Log(L"FEL POST-RGB INIT FAIL reason=CLIP_STATS_BUFFER hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC clipUavDesc{};
    clipUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    clipUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    clipUavDesc.Buffer.FirstElement = 0;
    clipUavDesc.Buffer.NumElements = 4;
    clipUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;

    hr = device->CreateUnorderedAccessView(
        g_postRgbClipStats,
        &clipUavDesc,
        &g_postRgbClipStatsUav);

    if (FAILED(hr) || !g_postRgbClipStatsUav) {
        Log(L"FEL POST-RGB INIT FAIL reason=CLIP_STATS_UAV hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    D3D11_BUFFER_DESC readbackDesc{};
    readbackDesc.ByteWidth = sizeof(clipZeros);
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device->CreateBuffer(
        &readbackDesc,
        nullptr,
        &g_postRgbClipStatsReadback);

    if (FAILED(hr) || !g_postRgbClipStatsReadback) {
        Log(L"FEL POST-RGB INIT FAIL reason=CLIP_STATS_READBACK hr=0x%08X",
            static_cast<unsigned>(hr));
        device3->Release();
        ReleasePostDolbyRgbState();
        return false;
    }

    g_postRgbClipWindowFrames = 0;
    g_postRgbClipWindowsLogged = 0;

    device->AddRef();
    g_postRgbDevice = device;
    g_postRgbDevice3 = device3;
    g_postRgbWidth = width;
    g_postRgbHeight = height;
    g_postRgbElWidth = elWidth;
    g_postRgbElHeight = elHeight;

    InterlockedExchange(&g_gpuRendererReady, 1);

    Log(L"FEL GPU READY api=D3D11_COMPUTE cs=5_0 path=POST_DOLBY_R10_RGB_PQ_LMS_EXACT_CLIP_DIAG size=%ux%u EL=%ux%u",
        width, height, elWidth, elHeight);
    return true;
}

static bool BuildPostDolbyMatrices(const DolbyColorInfo& color, float post[3][3], float inv[3][3]) {
    static const double lms2rgb[3][3] = {
        { 3.06441879, -2.16597676,  0.10155818 },
        {-0.65612108,  1.78554118, -0.12943749 },
        { 0.01736321, -0.04725154,  1.03004253 }
    };

    double m[3][3]{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k)
                m[r][c] += lms2rgb[r][k] * static_cast<double>(color.linear[k][c]);
            post[r][c] = static_cast<float>(m[r][c]);
        }
    }

    const double det =
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

    if (std::fabs(det) < 1e-12)
        return false;

    const double d = 1.0 / det;
    inv[0][0] = static_cast<float>((m[1][1] * m[2][2] - m[1][2] * m[2][1]) * d);
    inv[0][1] = static_cast<float>((m[0][2] * m[2][1] - m[0][1] * m[2][2]) * d);
    inv[0][2] = static_cast<float>((m[0][1] * m[1][2] - m[0][2] * m[1][1]) * d);
    inv[1][0] = static_cast<float>((m[1][2] * m[2][0] - m[1][0] * m[2][2]) * d);
    inv[1][1] = static_cast<float>((m[0][0] * m[2][2] - m[0][2] * m[2][0]) * d);
    inv[1][2] = static_cast<float>((m[0][2] * m[1][0] - m[0][0] * m[1][2]) * d);
    inv[2][0] = static_cast<float>((m[1][0] * m[2][1] - m[1][1] * m[2][0]) * d);
    inv[2][1] = static_cast<float>((m[0][1] * m[2][0] - m[0][0] * m[2][1]) * d);
    inv[2][2] = static_cast<float>((m[0][0] * m[1][1] - m[0][1] * m[1][0]) * d);
    return true;
}

static SRWLOCK g_hdr10BlLock = SRWLOCK_INIT;
struct Hdr10BlFrame {
    LONGLONG time = -1;
    ID3D11Texture2D* texture = nullptr;
};
static std::deque<Hdr10BlFrame> g_hdr10BlFrames;

static void CaptureHdr10BlInput(IMFSample* sample, LONGLONG time) {
    if (!sample || time < 0) return;
    ID3D11Texture2D* tex = nullptr;
    UINT subresource = 0;
    if (!GetSampleDxgiTexture(sample, &tex, &subresource)) return;
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_P010) { tex->Release(); return; }
    ID3D11Device* device = nullptr;
    tex->GetDevice(&device);
    if (!device) { tex->Release(); return; }
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    ID3D11Texture2D* snapshot = nullptr;
    if (ctx) {
        D3D11_TEXTURE2D_DESC copy = desc;
        copy.ArraySize = 1;
        copy.MipLevels = 1;
        copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copy.MiscFlags = 0;
        copy.CPUAccessFlags = 0;
        copy.Usage = D3D11_USAGE_DEFAULT;
        if (SUCCEEDED(device->CreateTexture2D(&copy, nullptr, &snapshot)) && snapshot)
            ctx->CopySubresourceRegion(snapshot, 0, 0, 0, 0, tex, subresource, nullptr);
        ctx->Release();
    }
    device->Release();
    tex->Release();
    if (!snapshot) return;
    AcquireSRWLockExclusive(&g_hdr10BlLock);
    for (auto it = g_hdr10BlFrames.begin(); it != g_hdr10BlFrames.end(); ++it) {
        if (it->time == time) {
            it->texture->Release();
            g_hdr10BlFrames.erase(it);
            break;
        }
    }
    g_hdr10BlFrames.push_back({ time, snapshot });
    while (g_hdr10BlFrames.size() > 48) {
        g_hdr10BlFrames.front().texture->Release();
        g_hdr10BlFrames.pop_front();
    }
    ReleaseSRWLockExclusive(&g_hdr10BlLock);
}

static ID3D11Texture2D* TakeHdr10BlInput(LONGLONG time) {
    AcquireSRWLockExclusive(&g_hdr10BlLock);
    ID3D11Texture2D* found = nullptr;
    for (auto it = g_hdr10BlFrames.begin(); it != g_hdr10BlFrames.end(); ++it) {
        if (it->time == time) {
            found = it->texture;
            g_hdr10BlFrames.erase(it);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_hdr10BlLock);
    return found;
}

static bool ApplyFelToPostDolbyRgb(
    ID3D11Texture2D* source,
    UINT sourceSubresource,
    const CapturedDvAccessUnit& au,
    const DecodedElFrame& el,
    const FelNlqCoeff nlq[3],
    LONGLONG sampleTime,
    LONG attempt) {

    if (!source || !el.width || !el.height || !au.rpu.color.valid) {
        Log(L"FEL POST-RGB FAIL attempt=%ld time=%lld reason=INVALID_INPUT color=%s",
            attempt,
            static_cast<long long>(sampleTime),
            au.rpu.color.valid ? L"YES" : L"NO");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);

    if (desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM ||
        desc.Width != el.width * 2u ||
        desc.Height != el.height * 2u) {
        return false;
    }

    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
    if (!device) return false;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        device->Release();
        return false;
    }

    LARGE_INTEGER perfFreq{}, perfStart{}, perfEnd{};
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&perfStart);

    ID3D11Texture2D* hdr10Frame = g_felSplitEnabled ? TakeHdr10BlInput(sampleTime) : nullptr;
    bool ok = false;
    AcquireSRWLockExclusive(&g_gpuFelLock);

    if (EnsurePostDolbyRgbState(
            device,
            desc.Width,
            desc.Height,
            el.width,
            el.height)) {

        context->CopySubresourceRegion(
            g_postRgbInput,
            0,
            0, 0, 0,
            source,
            sourceSubresource,
            nullptr);

        context->UpdateSubresource(
            g_postRgbElInput,
            0,
            nullptr,
            el.bytes.data(),
            el.width * 2u,
            static_cast<UINT>(el.bytes.size()));

        bool hdr10Ready = false;
        if (hdr10Frame && g_felSplitEnabled) {
            D3D11_TEXTURE2D_DESC inputHdrDesc{};
            hdr10Frame->GetDesc(&inputHdrDesc);
            ID3D11Device* hdrDev = nullptr;
            hdr10Frame->GetDevice(&hdrDev);
            hdr10Ready = hdrDev == device && inputHdrDesc.Format == DXGI_FORMAT_P010 &&
                         inputHdrDesc.Width == desc.Width && inputHdrDesc.Height == desc.Height;
            if (hdrDev) hdrDev->Release();
            if (hdr10Ready) context->CopyResource(g_postRgbHdr10Input, hdr10Frame);
        }
        PostDolbyRgbConstants constants{};
        constants.width = desc.Width;
        constants.height = desc.Height;
        constants.elWidth = el.width;
        constants.elHeight = el.height;
        constants.splitMode = static_cast<UINT>(g_felSplitEnabled);
        constants.hdr10Available = hdr10Ready ? 1u : 0u;
        if (attempt <= 24) Log(L"FEL HDR10 SPLIT frame=%ld time=%lld BLcapture=%s HDR10_LEFT=%s", attempt, static_cast<long long>(sampleTime), hdr10Frame ? L"YES" : L"NO", hdr10Ready ? L"YES" : L"NO");
        constants.lumaGuardMode = g_felHighlightLimitEnabled ? 1u : 0u;

        for (int c = 0; c < 3; ++c) {
            constants.offsetCode[c] = nlq[c].offsetCode;
            constants.slope[c] = nlq[c].slope;
            constants.threshold[c] = nlq[c].threshold;
        }

        for (int c = 0; c < 3; ++c) {
            constants.nonlinear0[c] = au.rpu.color.nonlinear[0][c];
            constants.nonlinear1[c] = au.rpu.color.nonlinear[1][c];
            constants.nonlinear2[c] = au.rpu.color.nonlinear[2][c];
        }

        float post[3][3]{};
        float invPost[3][3]{};
        if (!BuildPostDolbyMatrices(au.rpu.color, post, invPost)) {
            ReleaseSRWLockExclusive(&g_gpuFelLock);
            if (hdr10Frame) hdr10Frame->Release();
            context->Release();
            device->Release();
            return false;
        }
        for (int c = 0; c < 3; ++c) {
            constants.post0[c] = post[0][c];
            constants.post1[c] = post[1][c];
            constants.post2[c] = post[2][c];
            constants.invPost0[c] = invPost[0][c];
            constants.invPost1[c] = invPost[1][c];
            constants.invPost2[c] = invPost[2][c];
        }

        context->UpdateSubresource(
            g_postRgbConstants,
            0,
            nullptr,
            &constants,
            0,
            0);

        ID3D11ShaderResourceView* srvs[5] = {
            g_postRgbInputSrv, g_postRgbElYSrv, g_postRgbElUvSrv,
            g_postRgbHdr10YSrv, g_postRgbHdr10UvSrv
        };
        ID3D11UnorderedAccessView* uavs[2] = {
            g_postRgbOutputUav,
            g_postRgbClipStatsUav
        };

        context->CSSetShader(g_postRgbShader, nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &g_postRgbConstants);
        context->CSSetShaderResources(0, 5, srvs);
        context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

        context->Dispatch(
            (desc.Width + 15u) / 16u,
            (desc.Height + 15u) / 16u,
            1);

        ID3D11ShaderResourceView* nullSrvs[5] = {};
        ID3D11UnorderedAccessView* nullUavs[2] = {};
        ID3D11Buffer* nullCb[1] = {};

        context->CSSetShaderResources(0, 5, nullSrvs);
        context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        context->CSSetConstantBuffers(0, 1, nullCb);
        context->CSSetShader(nullptr, nullptr, 0);

        context->CopySubresourceRegion(
            source,
            sourceSubresource,
            0, 0, 0,
            g_postRgbOutput,
            0,
            nullptr);

        ++g_postRgbClipWindowFrames;
        const UINT clipWindowTarget =
            g_postRgbClipWindowsLogged == 0 ? 24u : 120u;

        if (g_postRgbClipWindowFrames >= clipWindowTarget &&
            g_postRgbClipStats &&
            g_postRgbClipStatsReadback &&
            g_postRgbClipStatsUav) {

            context->CopyResource(
                g_postRgbClipStatsReadback,
                g_postRgbClipStats);

            D3D11_MAPPED_SUBRESOURCE mappedClip{};
            const HRESULT clipMapHr = context->Map(
                g_postRgbClipStatsReadback,
                0,
                D3D11_MAP_READ,
                0,
                &mappedClip);

            if (SUCCEEDED(clipMapHr) && mappedClip.pData) {
                const UINT* stats =
                    static_cast<const UINT*>(mappedClip.pData);

                const UINT clippedPixels = stats[0];
                const UINT lowPixels = stats[1];
                const UINT highPixels = stats[2];
                const UINT lumaGuardPixels = stats[3];

                context->Unmap(g_postRgbClipStatsReadback, 0);

                const unsigned long long totalPixels =
                    static_cast<unsigned long long>(desc.Width) *
                    static_cast<unsigned long long>(desc.Height) *
                    static_cast<unsigned long long>(g_postRgbClipWindowFrames);

                const double ratio = totalPixels > 0
                    ? static_cast<double>(clippedPixels) /
                      static_cast<double>(totalPixels)
                    : 0.0;

                Log(L"FEL CLIP STATS frames=%u pixels=%llu clipped=%u low=%u high=%u lumaGuarded=%u ratio=%.9f percent=%.6f",
                    g_postRgbClipWindowFrames,
                    totalPixels,
                    clippedPixels,
                    lowPixels,
                    highPixels,
                    lumaGuardPixels,
                    ratio,
                    ratio * 100.0);
            } else {
                Log(L"FEL CLIP STATS readback=FAIL hr=0x%08X",
                    static_cast<unsigned>(clipMapHr));
            }

            const UINT clearClipStats[4] = {0, 0, 0, 0};
            context->ClearUnorderedAccessViewUint(
                g_postRgbClipStatsUav,
                clearClipStats);

            g_postRgbClipWindowFrames = 0;
            ++g_postRgbClipWindowsLogged;
        }

        ok = true;
    }

    ReleaseSRWLockExclusive(&g_gpuFelLock);
    if (hdr10Frame) hdr10Frame->Release();

    QueryPerformanceCounter(&perfEnd);

    context->Release();
    device->Release();

    if (!ok) return false;

    const double submitMs = perfFreq.QuadPart > 0
        ? (1000.0 * static_cast<double>(perfEnd.QuadPart - perfStart.QuadPart) /
           static_cast<double>(perfFreq.QuadPart))
        : 0.0;

    const LONG applied = InterlockedIncrement(&g_felReconApplied);
    InterlockedIncrement(&g_gpuAppliedFrames);

    if (applied <= 24 || (applied % 240) == 0) {
        Log(L"FEL GPU RECON DISPATCH frame=%ld time=%lld submitMs=%.3f BL=%ux%u EL=%ux%u ELhash=%016llX split=%s SAMPLE=POST_DOLBY_R10_RGB_PQ_LMS_EXACT_CLIP_DIAG",
            applied,
            static_cast<long long>(sampleTime),
            submitMs,
            desc.Width,
            desc.Height,
            el.width,
            el.height,
            static_cast<unsigned long long>(el.hash),
            g_felSplitEnabled == 2 ? L"DIFF" : (g_felSplitEnabled ? L"ON" : L"OFF"));
    }

    return true;
}

static bool EnsurePostDolbyGpuDevice() {
    if (g_postDolbyDevice && g_postDolbyContext) return true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        requested,
        static_cast<UINT>(_countof(requested)),
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context);

    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            &requested[1],
            1,
            D3D11_SDK_VERSION,
            &device,
            &featureLevel,
            &context);
    }

    if (FAILED(hr) || !device || !context) {
        if (context) context->Release();
        if (device) device->Release();
        Log(L"FEL POST-DOLBY GPU DEVICE FAIL hr=0x%08X",
            static_cast<unsigned>(hr));
        return false;
    }

    g_postDolbyDevice = device;
    g_postDolbyContext = context;

    Log(L"FEL POST-DOLBY GPU DEVICE READY featureLevel=0x%04X",
        static_cast<unsigned>(featureLevel));
    return true;
}

static bool EnsurePostDolbyReadback(UINT width, UINT height) {
    if (!g_postDolbyDevice || !width || !height) return false;

    if (g_postDolbyReadback &&
        g_postDolbyReadbackWidth == width &&
        g_postDolbyReadbackHeight == height) {
        return true;
    }

    if (g_postDolbyReadback) {
        g_postDolbyReadback->Release();
        g_postDolbyReadback = nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_P010;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = g_postDolbyDevice->CreateTexture2D(
        &desc, nullptr, &g_postDolbyReadback);

    if (FAILED(hr) || !g_postDolbyReadback) {
        Log(L"FEL POST-DOLBY READBACK FAIL hr=0x%08X",
            static_cast<unsigned>(hr));
        return false;
    }

    g_postDolbyReadbackWidth = width;
    g_postDolbyReadbackHeight = height;
    return true;
}

static bool ApplyFelReconstructionSystemMemory(
    IMFSample* sample,
    const CapturedDvAccessUnit& au,
    const DecodedElFrame& el,
    const FelNlqCoeff nlq[3],
    LONGLONG sampleTime,
    LONG attempt) {

    if (!sample || !el.width || !el.height) return false;

    const UINT width = el.width * 2u;
    const UINT height = el.height * 2u;
    const UINT rowBytes = width * 2u;
    const UINT rows = height + height / 2u;

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr) || !buffer) {
        Log(L"FEL POST-DOLBY SYSMEM FAIL attempt=%ld time=%lld reason=NO_BUFFER hr=0x%08X",
            attempt, static_cast<long long>(sampleTime), static_cast<unsigned>(hr));
        return false;
    }

    IMF2DBuffer2* twoD = nullptr;
    BYTE* scan0 = nullptr;
    BYTE* bufferStart = nullptr;
    LONG pitch = 0;
    DWORD bufferLength = 0;
    bool locked2D = false;
    bool lockedPlain = false;

    hr = buffer->QueryInterface(IID_PPV_ARGS(&twoD));
    if (SUCCEEDED(hr) && twoD) {
        hr = twoD->Lock2DSize(
            MF2DBuffer_LockFlags_ReadWrite,
            &scan0,
            &pitch,
            &bufferStart,
            &bufferLength);
        locked2D = SUCCEEDED(hr) && scan0;
    }

    if (!locked2D) {
        if (twoD) {
            twoD->Release();
            twoD = nullptr;
        }

        BYTE* data = nullptr;
        DWORD maxLen = 0;
        DWORD curLen = 0;
        hr = buffer->Lock(&data, &maxLen, &curLen);
        if (SUCCEEDED(hr) && data) {
            scan0 = data;
            pitch = static_cast<LONG>(rowBytes);
            bufferStart = data;
            bufferLength = curLen;
            lockedPlain = true;
        }
    }

    const size_t required =
        static_cast<size_t>(rowBytes) * static_cast<size_t>(rows);

    if ((!locked2D && !lockedPlain) ||
        !scan0 ||
        pitch <= 0 ||
        static_cast<UINT>(pitch) < rowBytes ||
        bufferLength < required) {

        Log(L"FEL POST-DOLBY SYSMEM FAIL attempt=%ld time=%lld reason=LOCK_OR_LAYOUT pitch=%ld len=%lu need=%llu",
            attempt,
            static_cast<long long>(sampleTime),
            pitch,
            bufferLength,
            static_cast<unsigned long long>(required));

        if (locked2D && twoD) twoD->Unlock2D();
        if (lockedPlain) buffer->Unlock();
        if (twoD) twoD->Release();
        buffer->Release();
        return false;
    }

    if (!EnsurePostDolbyGpuDevice()) {
        if (locked2D && twoD) twoD->Unlock2D();
        if (lockedPlain) buffer->Unlock();
        if (twoD) twoD->Release();
        buffer->Release();
        return false;
    }

    LARGE_INTEGER perfFreq{}, perfStart{}, perfEnd{};
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&perfStart);

    bool ok = false;
    AcquireSRWLockExclusive(&g_gpuFelLock);

    D3D11_TEXTURE2D_DESC sourceDesc{};
    sourceDesc.Width = width;
    sourceDesc.Height = height;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_P010;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_DEFAULT;

    if (EnsureGpuFelState(
            g_postDolbyDevice,
            sourceDesc,
            el.width,
            el.height) &&
        EnsurePostDolbyReadback(width, height)) {

        g_postDolbyContext->UpdateSubresource(
            g_gpuBlInput,
            0,
            nullptr,
            scan0,
            static_cast<UINT>(pitch),
            static_cast<UINT>(static_cast<size_t>(pitch) * rows));

        const UINT elRowPitch = el.width * 2u;
        const UINT elDepthPitch = static_cast<UINT>(el.bytes.size());

        g_postDolbyContext->UpdateSubresource(
            g_gpuElInput,
            0,
            nullptr,
            el.bytes.data(),
            elRowPitch,
            elDepthPitch);

        GpuFelConstants constants{};
        constants.blWidth = width;
        constants.blHeight = height;
        constants.elWidth = el.width;
        constants.elHeight = el.height;

        constants.carrierMode = 0u;
        constants.splitMode = static_cast<UINT>(g_felSplitEnabled);

        for (int i = 0; i < 3; ++i) {
            constants.offsetCode[i] = nlq[i].offsetCode;
            constants.slope[i] = nlq[i].slope;
            constants.threshold[i] = nlq[i].threshold;
        }

        g_postDolbyContext->UpdateSubresource(
            g_gpuFelConstants, 0, nullptr, &constants, 0, 0);

        ID3D11ShaderResourceView* srvs[4] = {
            g_gpuBlYSrv,
            g_gpuBlUvSrv,
            g_gpuElYSrv,
            g_gpuElUvSrv
        };

        ID3D11UnorderedAccessView* uavs[2] = {
            g_gpuOutYUav,
            g_gpuOutUvUav
        };

        g_postDolbyContext->CSSetShader(g_gpuFelShader, nullptr, 0);
        g_postDolbyContext->CSSetConstantBuffers(0, 1, &g_gpuFelConstants);
        g_postDolbyContext->CSSetShaderResources(0, 4, srvs);
        g_postDolbyContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

        const UINT groupsX = (width + 15u) / 16u;
        const UINT groupsY = (height + 15u) / 16u;
        g_postDolbyContext->Dispatch(groupsX, groupsY, 1);

        ID3D11ShaderResourceView* nullSrvs[4] = {};
        ID3D11UnorderedAccessView* nullUavs[2] = {};
        ID3D11Buffer* nullCb[1] = {};

        g_postDolbyContext->CSSetShaderResources(0, 4, nullSrvs);
        g_postDolbyContext->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        g_postDolbyContext->CSSetConstantBuffers(0, 1, nullCb);
        g_postDolbyContext->CSSetShader(nullptr, nullptr, 0);

        g_postDolbyContext->CopyResource(
            g_postDolbyReadback,
            g_gpuBlOutput);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = g_postDolbyContext->Map(
            g_postDolbyReadback,
            0,
            D3D11_MAP_READ,
            0,
            &mapped);

        if (SUCCEEDED(hr) && mapped.pData) {
            const BYTE* src = static_cast<const BYTE*>(mapped.pData);
            for (UINT y = 0; y < rows; ++y) {
                memcpy(
                    scan0 + static_cast<size_t>(pitch) * y,
                    src + static_cast<size_t>(mapped.RowPitch) * y,
                    rowBytes);
            }
            g_postDolbyContext->Unmap(g_postDolbyReadback, 0);
            ok = true;
        } else {
            Log(L"FEL POST-DOLBY SYSMEM FAIL attempt=%ld time=%lld reason=READBACK_MAP hr=0x%08X",
                attempt,
                static_cast<long long>(sampleTime),
                static_cast<unsigned>(hr));
        }
    }

    ReleaseSRWLockExclusive(&g_gpuFelLock);

    if (locked2D && twoD) twoD->Unlock2D();
    if (lockedPlain) buffer->Unlock();
    if (twoD) twoD->Release();
    buffer->Release();

    QueryPerformanceCounter(&perfEnd);

    if (!ok) return false;

    const double submitMs = perfFreq.QuadPart > 0
        ? (1000.0 * static_cast<double>(perfEnd.QuadPart - perfStart.QuadPart) /
           static_cast<double>(perfFreq.QuadPart))
        : 0.0;

    const LONG applied = InterlockedIncrement(&g_felReconApplied);
    InterlockedIncrement(&g_gpuAppliedFrames);

    if (applied <= 24 || (applied % 240) == 0) {
        Log(L"FEL GPU RECON DISPATCH frame=%ld time=%lld submitMs=%.3f BL=%ux%u EL=%ux%u ELhash=%016llX split=%s SAMPLE=POST_DOLBY_SYSMEM_GPU CARRIER=P010_STANDARD",
            applied,
            static_cast<long long>(sampleTime),
            submitMs,
            width,
            height,
            el.width,
            el.height,
            static_cast<unsigned long long>(el.hash),
            g_felSplitEnabled == 2 ? L"DIFF" : (g_felSplitEnabled ? L"ON" : L"OFF"));
    }

    return true;
}

static bool ApplyFelReconstruction(IMFSample* blSample, const CapturedDvAccessUnit& au) {
    const LONG attempt = InterlockedIncrement(&g_felReconAttempts);
    if (!blSample) return false;
    RefreshFelSplitMode();

    FelNlqCoeff nlq[3]{};
    if (!BuildFelNlqCoefficients(au.rpu, nlq)) {
        InterlockedIncrement(&g_felReconMisses);
        return false;
    }

    LONGLONG sampleTime = -1;
    blSample->GetSampleTime(&sampleTime);

    DecodedElFrame el;
    if (!FindElFrameByTime(sampleTime, el) ||
        el.subtype != MFVideoFormat_P010 ||
        !el.width || !el.height ||
        el.bytes.empty()) {

        const LONG miss = InterlockedIncrement(&g_felReconMisses);
        if (miss <= 24) {
            Log(L"FEL GPU RECON MISS attempt=%ld time=%lld reason=EL_NOT_EXACT",
                attempt, static_cast<long long>(sampleTime));
        }
        return false;
    }

    ID3D11Texture2D* source = nullptr;
    UINT sourceSubresource = 0;
    if (!GetSampleDxgiTexture(blSample, &source, &sourceSubresource)) {
        const bool fallbackOk = ApplyFelReconstructionSystemMemory(
            blSample,
            au,
            el,
            nlq,
            sampleTime,
            attempt);

        if (!fallbackOk) {
            InterlockedIncrement(&g_felReconFailures);
        }
        return fallbackOk;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);

    if (sourceDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
        sourceDesc.Width == el.width * 2u &&
        sourceDesc.Height == el.height * 2u) {

        const bool rgbOk = ApplyFelToPostDolbyRgb(
            source,
            sourceSubresource,
            au,
            el,
            nlq,
            sampleTime,
            attempt);

        source->Release();

        if (!rgbOk) {
            InterlockedIncrement(&g_felReconFailures);
        }
        return rgbOk;
    }

    if (sourceDesc.Format != DXGI_FORMAT_P010 ||
        sourceDesc.Width != el.width * 2u ||
        sourceDesc.Height != el.height * 2u) {

        Log(L"FEL GPU RECON MISS attempt=%ld time=%lld reason=DXGI_LAYOUT format=%u size=%ux%u expected=%ux%u",
            attempt,
            static_cast<long long>(sampleTime),
            static_cast<unsigned>(sourceDesc.Format),
            sourceDesc.Width,
            sourceDesc.Height,
            el.width * 2u,
            el.height * 2u);

        source->Release();
        InterlockedIncrement(&g_felReconMisses);
        return false;
    }

    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
    if (!device) {
        source->Release();
        InterlockedIncrement(&g_felReconFailures);
        return false;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        device->Release();
        source->Release();
        InterlockedIncrement(&g_felReconFailures);
        return false;
    }

    LARGE_INTEGER perfFreq{};
    LARGE_INTEGER perfStart{};
    LARGE_INTEGER perfEnd{};
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&perfStart);

    AcquireSRWLockExclusive(&g_gpuFelLock);

    if (!EnsureGpuFelState(device, sourceDesc, el.width, el.height)) {
        ReleaseSRWLockExclusive(&g_gpuFelLock);
        context->Release();
        device->Release();
        source->Release();
        InterlockedIncrement(&g_felReconFailures);
        return false;
    }

    context->CopySubresourceRegion(
        g_gpuBlInput, 0, 0, 0, 0,
        source, sourceSubresource, nullptr);

    const UINT elRowPitch = el.width * 2u;
    const UINT elDepthPitch = static_cast<UINT>(el.bytes.size());
    context->UpdateSubresource(
        g_gpuElInput, 0, nullptr,
        el.bytes.data(),
        elRowPitch,
        elDepthPitch);

    GpuFelConstants constants{};
    constants.blWidth = sourceDesc.Width;
    constants.blHeight = sourceDesc.Height;
    constants.elWidth = el.width;
    constants.elHeight = el.height;
    constants.carrierMode = g_low6CarrierEnabled ? 1u : 0u;
    constants.splitMode = static_cast<UINT>(g_felSplitEnabled);

    for (int i = 0; i < 3; ++i) {
        constants.offsetCode[i] = nlq[i].offsetCode;
        constants.slope[i] = nlq[i].slope;
        constants.threshold[i] = nlq[i].threshold;
    }

    context->UpdateSubresource(g_gpuFelConstants, 0, nullptr, &constants, 0, 0);

    ID3D11ShaderResourceView* srvs[4] = {
        g_gpuBlYSrv,
        g_gpuBlUvSrv,
        g_gpuElYSrv,
        g_gpuElUvSrv
    };

    ID3D11UnorderedAccessView* uavs[2] = {
        g_gpuOutYUav,
        g_gpuOutUvUav
    };

    context->CSSetShader(g_gpuFelShader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &g_gpuFelConstants);
    context->CSSetShaderResources(0, 4, srvs);
    context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

    const UINT groupsX = (sourceDesc.Width + 15u) / 16u;
    const UINT groupsY = (sourceDesc.Height + 15u) / 16u;
    context->Dispatch(groupsX, groupsY, 1);

    ID3D11ShaderResourceView* nullSrvs[4] = {};
    ID3D11UnorderedAccessView* nullUavs[2] = {};
    ID3D11Buffer* nullCb[1] = {};
    context->CSSetShaderResources(0, 4, nullSrvs);
    context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
    context->CSSetConstantBuffers(0, 1, nullCb);
    context->CSSetShader(nullptr, nullptr, 0);

    if (!g_referenceInitialElHash) g_referenceInitialElHash = el.hash;
    const bool dynamicEl = g_referenceInitialElHash && el.hash != g_referenceInitialElHash;
    if (dynamicEl && InterlockedCompareExchange(&g_referenceCaptureDone, 1, 0) == 0) {
        const bool blOk = DumpP010TextureTight(device, context, g_gpuBlInput, L"FEL-REF-BL-before.p010");
        const bool outOk = DumpP010TextureTight(device, context, g_gpuBlOutput, L"FEL-REF-GPU-after.p010");
        const bool elOk = WriteBinarySibling(L"FEL-REF-EL.p010", el.bytes.data(), el.bytes.size());
        uint64_t low6Nonzero = 0, low6Total = 0;
        const bool low6Ok = CountP010Low6(device, context, g_gpuBlOutput, &low6Nonzero, &low6Total);
        WriteReferenceMetadata(sampleTime, sourceDesc, el, nlq);
        if (low6Ok) {
            Log(L"FEL 12BIT CARRIER low6Nonzero=%llu low6Total=%llu ratio=%.9f", 
                static_cast<unsigned long long>(low6Nonzero),
                static_cast<unsigned long long>(low6Total),
                low6Total ? static_cast<double>(low6Nonzero) / static_cast<double>(low6Total) : 0.0);
        } else {
            Log(L"FEL 12BIT CARRIER low6 probe failed");
        }
        Log(L"FEL REFERENCE CAPTURE ready=%s time=%lld ELhash=%016llX BL=%s EL=%s GPU=%s",
            (blOk && elOk && outOk) ? L"YES" : L"PARTIAL",
            static_cast<long long>(sampleTime),
            static_cast<unsigned long long>(el.hash),
            blOk ? L"OK" : L"FAIL", elOk ? L"OK" : L"FAIL", outOk ? L"OK" : L"FAIL");
    }

    context->CopySubresourceRegion(
        source, sourceSubresource, 0, 0, 0,
        g_gpuBlOutput, 0, nullptr);

    ReleaseSRWLockExclusive(&g_gpuFelLock);

    QueryPerformanceCounter(&perfEnd);
    const double submitMs = perfFreq.QuadPart > 0
        ? (1000.0 * static_cast<double>(perfEnd.QuadPart - perfStart.QuadPart) /
           static_cast<double>(perfFreq.QuadPart))
        : 0.0;

    context->Release();
    device->Release();
    source->Release();

    const LONG applied = InterlockedIncrement(&g_felReconApplied);
    InterlockedIncrement(&g_gpuAppliedFrames);

    if (applied <= 24 || (applied % 240) == 0) {
        Log(L"FEL GPU RECON DISPATCH frame=%ld time=%lld submitMs=%.3f BL=%ux%u EL=%ux%u ELhash=%016llX split=%s SAMPLE=LIBPLACEBO_EXACT_LUT16_NATIVE_P010 CARRIER=%s S=%.9f,%.9f,%.9f T=%.9f,%.9f,%.9f",
            applied,
            static_cast<long long>(sampleTime),
            submitMs,
            sourceDesc.Width, sourceDesc.Height,
            el.width, el.height,
            static_cast<unsigned long long>(el.hash),
            g_felSplitEnabled == 2 ? L"DIFF" : (g_felSplitEnabled ? L"ON" : L"OFF"),
            g_low6CarrierEnabled ? L"P010_LOW6_12BIT" : L"P010_MASK10",
            nlq[0].slope, nlq[1].slope, nlq[2].slope,
            nlq[0].threshold, nlq[1].threshold, nlq[2].threshold);
    }

    return true;
}


static const wchar_t* MediaSubtypeName(const GUID& subtype) {
    if (subtype == MFVideoFormat_P010) return L"P010";
    if (subtype == MFVideoFormat_P016) return L"P016";
    if (subtype == MFVideoFormat_NV12) return L"NV12";
    return L"OTHER";
}

static void ProbeDolbyP016(IMFTransform* transform) {
    if (!transform) return;
    bool p016 = false;
    for (DWORD i = 0; i < 64; ++i) {
        IMFMediaType* type = nullptr;
        HRESULT hr = transform->GetInputAvailableType(0, i, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr) || !type) continue;
        GUID subtype = GUID_NULL;
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
        Log(L"FEL DOLBY INPUT TYPE index=%lu subtype=%s size=%ux%u", i, MediaSubtypeName(subtype), w, h);
        if (subtype == MFVideoFormat_P016) p016 = true;
        type->Release();
    }
    InterlockedExchange(&g_dolbyP016Available, p016 ? 1 : 0);
    Log(L"FEL 12BIT CAPABILITY P016=%s", p016 ? L"YES" : L"NO");
}

static bool SetDolbyInputSubtype(IMFTransform* transform, const GUID& subtype) {
    if (!transform) return false;
    IMFMediaType* current = nullptr;
    HRESULT hr = transform->GetInputCurrentType(0, &current);
    if (FAILED(hr) || !current) return false;
    GUID oldSubtype = GUID_NULL;
    current->GetGUID(MF_MT_SUBTYPE, &oldSubtype);
    if (oldSubtype == subtype) { current->Release(); return true; }

    IMFMediaType* next = nullptr;
    hr = MFCreateMediaType(&next);
    if (SUCCEEDED(hr) && next) hr = current->CopyAllItems(next);
    if (SUCCEEDED(hr) && next) hr = next->SetGUID(MF_MT_SUBTYPE, subtype);
    HRESULT testHr = FAILED(hr) ? hr : transform->SetInputType(0, next, MFT_SET_TYPE_TEST_ONLY);
    HRESULT setHr = FAILED(testHr) ? testHr : transform->SetInputType(0, next, 0);
    Log(L"FEL 12BIT SWITCH %s->%s testHr=0x%08X setHr=0x%08X",
        MediaSubtypeName(oldSubtype), MediaSubtypeName(subtype),
        static_cast<unsigned>(testHr), static_cast<unsigned>(setHr));
    if (SUCCEEDED(setHr)) InterlockedIncrement(&g_dolbyP016Switches);
    if (next) next->Release();
    current->Release();
    return SUCCEEDED(setHr);
}

static bool BuildFelP016Sample(IMFSample* sourceSample, const CapturedDvAccessUnit& au, IMFSample** outSample) {
    if (!sourceSample || !outSample) return false;
    *outSample = nullptr;

    FelNlqCoeff nlq[3]{};
    if (!BuildFelNlqCoefficients(au.rpu, nlq)) return false;
    LONGLONG sampleTime = -1;
    sourceSample->GetSampleTime(&sampleTime);
    DecodedElFrame el;
    if (!FindElFrameByTime(sampleTime, el) || el.subtype != MFVideoFormat_P010 || !el.width || !el.height || el.bytes.empty())
        return false;

    ID3D11Texture2D* source = nullptr;
    UINT sourceSubresource = 0;
    if (!GetSampleDxgiTexture(sourceSample, &source, &sourceSubresource)) return false;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_P010 || sourceDesc.Width != el.width * 2u || sourceDesc.Height != el.height * 2u) {
        source->Release();
        return false;
    }

    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    if (device) device->GetImmediateContext(&context);
    if (!device || !context) {
        if (context) context->Release();
        if (device) device->Release();
        source->Release();
        return false;
    }

    AcquireSRWLockExclusive(&g_gpuFelLock);
    bool ok = EnsureGpuFelState(device, sourceDesc, el.width, el.height);
    if (!ok) {
        ReleaseSRWLockExclusive(&g_gpuFelLock);
        context->Release(); device->Release(); source->Release();
        return false;
    }

    context->CopySubresourceRegion(g_gpuBlInput, 0, 0, 0, 0, source, sourceSubresource, nullptr);
    context->UpdateSubresource(g_gpuElInput, 0, nullptr, el.bytes.data(), el.width * 2u, static_cast<UINT>(el.bytes.size()));

    GpuFelConstants constants{};
    constants.blWidth = sourceDesc.Width;
    constants.blHeight = sourceDesc.Height;
    constants.elWidth = el.width;
    constants.elHeight = el.height;
    for (int i = 0; i < 3; ++i) {
        constants.offsetCode[i] = nlq[i].offsetCode;
        constants.slope[i] = nlq[i].slope;
        constants.threshold[i] = nlq[i].threshold;
    }
    context->UpdateSubresource(g_gpuFelConstants, 0, nullptr, &constants, 0, 0);

    D3D11_TEXTURE2D_DESC outDesc{};
    outDesc.Width = sourceDesc.Width;
    outDesc.Height = sourceDesc.Height;
    outDesc.MipLevels = 1;
    outDesc.ArraySize = 1;
    outDesc.Format = DXGI_FORMAT_P016;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* outTex = nullptr;
    HRESULT hr = device->CreateTexture2D(&outDesc, nullptr, &outTex);
    ID3D11UnorderedAccessView1* outY = nullptr;
    ID3D11UnorderedAccessView1* outUV = nullptr;
    if (SUCCEEDED(hr) && outTex) {
        ok = CreatePlaneUav(g_gpuFelDevice3, outTex, DXGI_FORMAT_R16_UNORM, 0, &outY) &&
             CreatePlaneUav(g_gpuFelDevice3, outTex, DXGI_FORMAT_R16G16_UNORM, 1, &outUV);
    } else ok = false;

    if (ok) {
        ID3D11ShaderResourceView* srvs[4] = { g_gpuBlYSrv, g_gpuBlUvSrv, g_gpuElYSrv, g_gpuElUvSrv };
        ID3D11UnorderedAccessView* uavs[2] = { outY, outUV };
        context->CSSetShader(g_gpuFelP016Shader, nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &g_gpuFelConstants);
        context->CSSetShaderResources(0, 4, srvs);
        context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        context->Dispatch((sourceDesc.Width + 15u) / 16u, (sourceDesc.Height + 15u) / 16u, 1);
        ID3D11ShaderResourceView* nullSrvs[4] = {};
        ID3D11UnorderedAccessView* nullUavs[2] = {};
        ID3D11Buffer* nullCb[1] = {};
        context->CSSetShaderResources(0, 4, nullSrvs);
        context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        context->CSSetConstantBuffers(0, 1, nullCb);
        context->CSSetShader(nullptr, nullptr, 0);
    }

    if (outY) outY->Release();
    if (outUV) outUV->Release();
    ReleaseSRWLockExclusive(&g_gpuFelLock);

    if (ok && outTex) {
        IMFMediaBuffer* dxgiBuffer = nullptr;
        IMFSample* replacement = nullptr;
        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), outTex, 0, FALSE, &dxgiBuffer);
        if (SUCCEEDED(hr)) hr = MFCreateSample(&replacement);
        if (SUCCEEDED(hr) && replacement && dxgiBuffer) hr = replacement->AddBuffer(dxgiBuffer);
        if (SUCCEEDED(hr) && replacement) {
            sourceSample->CopyAllItems(replacement);
            LONGLONG duration = 0;
            DWORD sampleFlags = 0;
            if (SUCCEEDED(sourceSample->GetSampleTime(&sampleTime))) replacement->SetSampleTime(sampleTime);
            if (SUCCEEDED(sourceSample->GetSampleDuration(&duration))) replacement->SetSampleDuration(duration);
            if (SUCCEEDED(sourceSample->GetSampleFlags(&sampleFlags))) replacement->SetSampleFlags(sampleFlags);
            *outSample = replacement;
            replacement = nullptr;
            InterlockedIncrement(&g_dolbyP016Frames);
        }
        if (replacement) replacement->Release();
        if (dxgiBuffer) dxgiBuffer->Release();
    }

    if (outTex) outTex->Release();
    context->Release(); device->Release(); source->Release();
    return *outSample != nullptr;
}

static void LogFelTriplet(IMFSample* blSample, const CapturedDvAccessUnit& au) {
    if (!blSample) return;

    LONGLONG blTime = -1;
    blSample->GetSampleTime(&blTime);

    DXGI_FORMAT blFormat{};
    UINT blWidth = 0, blHeight = 0, subresource = 0;
    const bool blDxgi = ReadBlDxgiSurface(blSample, blFormat, blWidth, blHeight, subresource);
    if (blDxgi) InterlockedIncrement(&g_blDxgiFrames);

    DecodedElFrame el;
    LONGLONG newestElTime = -1;
    size_t queuedElFrames = 0;
    const bool elFound = FindElFrameByTime(blTime, el, &newestElTime, &queuedElFrames);
    const bool exact = blDxgi && blFormat == DXGI_FORMAT_P010 &&
        elFound && el.subtype == MFVideoFormat_P010 &&
        au.rpu.parsed && au.rpu.profile7 && au.rpu.fel && au.rpu.nlqActive;

    const LONG exactFrame = exact ? InterlockedIncrement(&g_felTripletFrames) :
        InterlockedCompareExchange(&g_felTripletFrames, 0, 0);
    const LONG probe = InterlockedIncrement(&g_tripletProbeCount);
    const LONGLONG elDelta = newestElTime >= 0 && blTime >= 0 ? blTime - newestElTime : -1;

    if (probe <= 160 || exact || (probe % 240) == 0) {
        Log(L"FEL TRIPLET probe=%ld time=%lld BL=%s %ux%u sub=%u EL=%s %ux%u ELNEWEST=%lld delta=%lld q=%llu RPU=%s P7=%s FEL=%s NLQ=%s exact=%s total=%ld",
            probe,
            static_cast<long long>(blTime),
            blDxgi ? DxgiFormatName(blFormat) : L"NO-DXGI",
            blWidth, blHeight, subresource,
            elFound ? ElSubtypeName(el.subtype) : L"MISSING",
            elFound ? el.width : 0,
            elFound ? el.height : 0,
            static_cast<long long>(newestElTime),
            static_cast<long long>(elDelta),
            static_cast<unsigned long long>(queuedElFrames),
            au.rpu.parsed ? L"YES" : L"NO",
            au.rpu.profile7 ? L"YES" : L"NO",
            au.rpu.fel ? L"YES" : L"NO",
            au.rpu.nlqActive ? L"YES" : L"NO",
            exact ? L"YES" : L"NO",
            exactFrame);
    }
}

static void BridgeFel63ToOutput(IMFSample* outputSample) {
    if (!outputSample) return;

    IMFCollection* oldCollection = nullptr;
    HRESULT hr = outputSample->GetUnknown(
        kForwardedDecodeUnits,
        __uuidof(IMFCollection),
        reinterpret_cast<void**>(&oldCollection));

    if (FAILED(hr) || !oldCollection) return;

    DWORD oldCount = 0;
    if (FAILED(oldCollection->GetElementCount(&oldCount)) || oldCount == 0) {
        oldCollection->Release();
        return;
    }

    LONGLONG outputTime = -1;
    outputSample->GetSampleTime(&outputTime);

    std::vector<BYTE> firstRpu;
    for (DWORD i = 0; i < oldCount && firstRpu.empty(); ++i) {
        IUnknown* unit = nullptr;
        if (SUCCEEDED(oldCollection->GetElement(i, &unit)) && unit) {
            UINT32 type = 0xFFFFFFFFu;
            std::vector<BYTE> bytes;
            if (ReadUnitBytes(unit, bytes, type) && type == kDolbyVisionHevcNaluType) {
                firstRpu = std::move(bytes);
            }
            unit->Release();
        }
    }

    CapturedDvAccessUnit matched;
    bool matchedByTime = false;
    const bool haveMatch = !firstRpu.empty() &&
        TakeCapturedAuForOutput(outputTime, firstRpu, matched, matchedByTime);

    if (!haveMatch) {
        const LONG miss = InterlockedIncrement(&g_felBridgeMissCount);
        if (miss <= 24) {
            size_t queueSize = 0;
            LONGLONG firstTime = -1;
            LONGLONG lastTime = -1;
            AcquireSRWLockShared(&g_felLock);
            queueSize = g_dvQueue.size();
            if (!g_dvQueue.empty()) {
                firstTime = g_dvQueue.front().sampleTime;
                lastTime = g_dvQueue.back().sampleTime;
            }
            ReleaseSRWLockShared(&g_felLock);

            Log(L"FEL BRIDGE miss=%ld outputTime=%lld oldUnits=%lu queue=%llu firstTime=%lld lastTime=%lld",
                miss,
                static_cast<long long>(outputTime),
                oldCount,
                static_cast<unsigned long long>(queueSize),
                static_cast<long long>(firstTime),
                static_cast<long long>(lastTime));
        }

        oldCollection->Release();
        return;
    }

    QueueDolbyFelAu(matched);

    LogFelTriplet(outputSample, matched);

    const LONG bridge = InterlockedIncrement(&g_felBridgeCount);
    if (bridge <= 64) {
        UINT32 suppressed63 = 0;
        for (const auto& unit : matched.units) {
            if (unit.nalType == kDolbyVisionFelNaluType) ++suppressed63;
        }

        Log(L"FEL BRIDGE output=%ld time=%lld oldUnits=%lu matchedAU=1 suppressedNAL63=%u finalUnits=%lu match=%s mode=BL_PLUS_RPU_TO_DOLBY__FEL_AFTER_DOLBY",
            bridge,
            static_cast<long long>(outputTime),
            oldCount,
            suppressed63,
            oldCount,
            matchedByTime ? L"PTS" : L"RPU");
    }

    oldCollection->Release();
}


static void Log(const wchar_t* fmt, ...) {
    AcquireSRWLockExclusive(&g_logLock);

    wchar_t logPath[32768]{};
    if (g_selfModule && GetModuleFileNameW(g_selfModule, logPath, _countof(logPath))) {
        wchar_t* slash = wcsrchr(logPath, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wcscat_s(logPath, L"P7-PROFILE-HOOK.log");
    } else {
        wcscpy_s(logPath, L"P7-PROFILE-HOOK.log");
    }

    HANDLE h = CreateFileW(logPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER logSize{};
        if (GetFileSizeEx(h, &logSize) && logSize.QuadPart == 0) {
            const wchar_t bom = 0xFEFF;
            DWORD bomWritten = 0;
            WriteFile(h, &bom, sizeof(bom), &bomWritten, nullptr);
        }

        SYSTEMTIME st{};
        GetLocalTime(&st);

        wchar_t msg[4096]{};
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, args);
        va_end(args);

        wchar_t line[4608]{};
        _snwprintf_s(line, _countof(line), _TRUNCATE,
            L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu %s\r\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId(), msg);

        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(h);
        OutputDebugStringW(line);
    }

    ReleaseSRWLockExclusive(&g_logLock);
}

static bool ReadFriendlyName(IMFActivate* activate, wchar_t* name, UINT32 cch) {
    if (!activate || !name || !cch) return false;
    UINT32 len = 0;
    return SUCCEEDED(activate->GetString(MFT_FRIENDLY_NAME_Attribute, name, cch, &len));
}

static bool IsDolbyRenderer(IMFActivate* activate) {
    wchar_t name[256]{};
    return ReadFriendlyName(activate, name, _countof(name)) &&
        _wcsicmp(name, L"Dolby Vision Extensions") == 0;
}

static bool IsHevcDecoder(IMFActivate* activate) {
    wchar_t name[256]{};
    return ReadFriendlyName(activate, name, _countof(name)) &&
        (_wcsicmp(name, L"HEVCVideoExtension") == 0 ||
         wcsstr(name, L"HEVCVideoExtension") != nullptr);
}

static std::wstring ReadProfiles(const PROPVARIANT& pv) {
    std::wstring s;
    if (pv.vt != (VT_VECTOR | VT_LPWSTR)) return L"<not-vector>";
    for (ULONG i = 0; i < pv.calpwstr.cElems; ++i) {
        if (i) s += L",";
        if (pv.calpwstr.pElems[i]) s += pv.calpwstr.pElems[i];
    }
    return s;
}

static bool HasProfile(const PROPVARIANT& pv, const wchar_t* wanted) {
    if (pv.vt != (VT_VECTOR | VT_LPWSTR)) return false;
    for (ULONG i = 0; i < pv.calpwstr.cElems; ++i) {
        const wchar_t* p = pv.calpwstr.pElems[i];
        if (p && _wcsicmp(p, wanted) == 0) return true;
    }
    return false;
}

static HRESULT SetProfilesWithP7(IMFActivate* activate) {
    PROPVARIANT oldPv{};
    PropVariantInit(&oldPv);

    HRESULT hr = activate->GetItem(MFT_ENUM_VIDEO_RENDERER_EXTENSION_PROFILE, &oldPv);
    if (FAILED(hr)) {
        Log(L"Dolby activation has no renderer profile vector hr=0x%08X", static_cast<unsigned>(hr));
        return hr;
    }

    if (oldPv.vt != (VT_VECTOR | VT_LPWSTR)) {
        Log(L"Dolby renderer profile attr unexpected vt=0x%04X", oldPv.vt);
        PropVariantClear(&oldPv);
        return E_UNEXPECTED;
    }

    const std::wstring before = ReadProfiles(oldPv);
    if (HasProfile(oldPv, L"dvhe.07")) {
        PropVariantClear(&oldPv);
        return S_OK;
    }

    const ULONG count = oldPv.calpwstr.cElems + 1;
    PROPVARIANT newPv{};
    PropVariantInit(&newPv);
    newPv.vt = VT_VECTOR | VT_LPWSTR;
    newPv.calpwstr.cElems = count;
    newPv.calpwstr.pElems = static_cast<LPWSTR*>(CoTaskMemAlloc(sizeof(LPWSTR) * count));
    if (!newPv.calpwstr.pElems) {
        PropVariantClear(&oldPv);
        return E_OUTOFMEMORY;
    }
    ZeroMemory(newPv.calpwstr.pElems, sizeof(LPWSTR) * count);

    HRESULT copyHr = S_OK;
    for (ULONG i = 0; i < oldPv.calpwstr.cElems; ++i) {
        const wchar_t* src = oldPv.calpwstr.pElems[i] ? oldPv.calpwstr.pElems[i] : L"";
        const size_t bytes = (wcslen(src) + 1) * sizeof(wchar_t);
        newPv.calpwstr.pElems[i] = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!newPv.calpwstr.pElems[i]) { copyHr = E_OUTOFMEMORY; break; }
        memcpy(newPv.calpwstr.pElems[i], src, bytes);
    }

    if (SUCCEEDED(copyHr)) {
        const wchar_t* p7 = L"dvhe.07";
        const size_t bytes = (wcslen(p7) + 1) * sizeof(wchar_t);
        newPv.calpwstr.pElems[count - 1] = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!newPv.calpwstr.pElems[count - 1]) copyHr = E_OUTOFMEMORY;
        else memcpy(newPv.calpwstr.pElems[count - 1], p7, bytes);
    }

    if (SUCCEEDED(copyHr)) {
        hr = activate->SetItem(MFT_ENUM_VIDEO_RENDERER_EXTENSION_PROFILE, newPv);
        Log(L"PATCH Dolby renderer profiles [%s] + dvhe.07 -> hr=0x%08X",
            before.c_str(), static_cast<unsigned>(hr));
    } else {
        hr = copyHr;
    }

    PropVariantClear(&newPv);
    PropVariantClear(&oldPv);
    return hr;
}

static bool IsHevcVideoType(IMFMediaType* type) {
    if (!type) return false;
    GUID major{};
    GUID subtype{};
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) return false;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) return false;
    return subtype == MFVideoFormat_HEVC || subtype == kHevcEsSubtype;
}

static bool IsMatroskaHevc(IMFMediaType* type) {
    if (!type) return false;

    GUID major{};
    GUID subtype{};
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) return false;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_HEVC) return false;

    wchar_t codecId[128]{};
    UINT32 len = 0;
    if (FAILED(type->GetString(kMatroskaCodecId, codecId, _countof(codecId), &len))) return false;
    return _wcsicmp(codecId, L"V_MPEGH/ISO/HEVC") == 0;
}

static bool IsP010OrNv12(IMFMediaType* type) {
    if (!type) return false;
    GUID major{};
    GUID subtype{};
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) return false;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) return false;
    return subtype == MFVideoFormat_P010 || subtype == MFVideoFormat_NV12;
}

static bool ReadRendererProfile(IMFMediaType* type, wchar_t* profile, UINT32 cch) {
    if (!type || !profile || cch == 0) return false;
    profile[0] = 0;
    UINT32 len = 0;
    return SUCCEEDED(type->GetString(kRendererExtensionProfile, profile, cch, &len)) && profile[0] != 0;
}

static bool IsP7CandidateDecoder(IMFTransform* transform) {
    if (!transform) return false;
    AcquireSRWLockShared(&g_p7CandidateLock);
    const bool found = g_p7CandidateDecoders.find(transform) != g_p7CandidateDecoders.end();
    ReleaseSRWLockShared(&g_p7CandidateLock);
    return found;
}

static void SetP7CandidateDecoder(IMFTransform* transform, bool enabled) {
    if (!transform) return;
    AcquireSRWLockExclusive(&g_p7CandidateLock);
    if (enabled) g_p7CandidateDecoders.insert(transform);
    else g_p7CandidateDecoders.erase(transform);
    ReleaseSRWLockExclusive(&g_p7CandidateLock);
}

static void ForceP7Profile(IMFMediaType* type, const wchar_t* where) {
    if (!type) return;
    HRESULT hr = type->SetString(kRendererExtensionProfile, L"dvhe.07");
    Log(L"FORCE %s MF_MT_VIDEO_RENDERER_EXTENSION_PROFILE=dvhe.07 hr=0x%08X",
        where, static_cast<unsigned>(hr));
}



static void LogDolbyInputForwardedUnits(IMFSample* sample, LONG frameIndex) {
    if (!sample) return;

    LONGLONG sampleTime = -1;
    sample->GetSampleTime(&sampleTime);

    IMFCollection* collection = nullptr;
    HRESULT hr = sample->GetUnknown(
        kForwardedDecodeUnits,
        __uuidof(IMFCollection),
        reinterpret_cast<void**>(&collection));

    if (FAILED(hr) || !collection) {
        if (frameIndex <= 16) {
            Log(L"FEL DOLBY_IN frame=%ld time=%lld ForwardedDecodeUnits=NULL hr=0x%08X",
                frameIndex,
                static_cast<long long>(sampleTime),
                static_cast<unsigned>(hr));
        }
        return;
    }

    DWORD count = 0;
    HRESULT countHr = collection->GetElementCount(&count);

    UINT32 rpu62 = 0;
    UINT32 el63 = 0;
    UINT64 rpuBytes = 0;
    UINT64 elBytes = 0;

    for (DWORD i = 0; i < count; ++i) {
        IUnknown* item = nullptr;
        if (FAILED(collection->GetElement(i, &item)) || !item) {
            continue;
        }

        std::vector<BYTE> bytes;
        UINT32 nalType = 0xFFFFFFFFu;

        if (ReadUnitBytes(item, bytes, nalType)) {
            if (nalType == kDolbyVisionHevcNaluType) {
                ++rpu62;
                rpuBytes += bytes.size();
            }
            else if (nalType == kDolbyVisionFelNaluType) {
                ++el63;
                elBytes += bytes.size();
            }
        }

        item->Release();
    }

    Log(L"FEL DOLBY_IN frame=%ld time=%lld units=%lu countHr=0x%08X RPU62=%u RPUbytes=%llu EL63=%u EL63bytes=%llu",
        frameIndex,
        static_cast<long long>(sampleTime),
        count,
        static_cast<unsigned>(countHr),
        rpu62,
        static_cast<unsigned long long>(rpuBytes),
        el63,
        static_cast<unsigned long long>(elBytes));

    collection->Release();
}

static HRESULT STDMETHODCALLTYPE HookDolbyProcessInput(
    IMFTransform* self,
    DWORD streamId,
    IMFSample* sample,
    DWORD flags) {

    const LONG frameIndex = InterlockedIncrement(&g_dolbyInputFramesLogged);
    if (sample && frameIndex <= 256) LogDolbyInputForwardedUnits(sample, frameIndex);

    LONGLONG sampleTime = -1;
    if (sample) sample->GetSampleTime(&sampleTime);

    if (g_felSplitEnabled != 0 && sample) CaptureHdr10BlInput(sample, sampleTime);

    HRESULT hr = g_originalDolbyProcessInput
        ? g_originalDolbyProcessInput(self, streamId, sample, flags)
        : E_UNEXPECTED;

    if (frameIndex <= 256) {
        Log(L"FEL DOLBY_IN frame=%ld ProcessInput original hr=0x%08X stream=%lu flags=0x%08X submitted=P010 mode=RESHAPE_FIRST",
            frameIndex, static_cast<unsigned>(hr), streamId, flags);
    }

    return hr;
}


static HRESULT STDMETHODCALLTYPE HookDolbyProcessOutput(
    IMFTransform* self,
    DWORD flags,
    DWORD outputBufferCount,
    MFT_OUTPUT_DATA_BUFFER* outputSamples,
    DWORD* status) {

    const LONG outputCall = InterlockedIncrement(&g_dolbyOutputCalls);

    HRESULT hr = g_originalDolbyProcessOutput
        ? g_originalDolbyProcessOutput(
            self,
            flags,
            outputBufferCount,
            outputSamples,
            status)
        : E_UNEXPECTED;

    if (SUCCEEDED(hr) && outputSamples) {
        for (DWORD i = 0; i < outputBufferCount; ++i) {
            IMFSample* sample = outputSamples[i].pSample;
            if (!sample) continue;

            const LONG frameIndex =
                InterlockedIncrement(&g_dolbyOutputFramesLogged);

            LONGLONG outTime = -1;
            const bool haveTime = SUCCEEDED(sample->GetSampleTime(&outTime));

            bool felApplied = false;
            CapturedDvAccessUnit matched{};
            if (haveTime && TakeDolbyFelAu(outTime, matched)) {
                felApplied = ApplyFelReconstruction(sample, matched);
            }

            if (frameIndex <= 64 || (frameIndex % 240) == 0) {
                Log(L"FEL DOLBY_OUT frame=%ld time=%lld postDolbyFEL=%s order=RESHAPE_THEN_NLQ_RGB_DELTA",
                    frameIndex,
                    static_cast<long long>(outTime),
                    felApplied ? L"YES" : L"NO");
            }
        }
    }

    if (outputCall <= 128) {
        Log(L"FEL DOLBY_OUT ProcessOutput call=%ld hr=0x%08X buffers=%lu status=%lu mode=%s",
            outputCall,
            static_cast<unsigned>(hr),
            outputBufferCount,
            status ? *status : 0,
            g_felBridgeEnabled ? L"ON" : L"OFF");
    }

    return hr;
}


static void InstallDolbyProofHooks(IMFActivate* activate) {
    if (!activate) return;
    if (InterlockedCompareExchange(&g_dolbyHookState, 1, 0) != 0) return;

    IMFTransform* transform = nullptr;
    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&transform));

    if (FAILED(hr) || !transform) {
        Log(L"ERROR: Dolby input proof ActivateObject failed hr=0x%08X",
            static_cast<unsigned>(hr));
        InterlockedExchange(&g_dolbyHookState, 0);
        return;
    }

    ProbeDolbyP016(transform);

    void** vtbl = *reinterpret_cast<void***>(transform);
    void* processInputTarget = vtbl[24];
    void* processOutputTarget = vtbl[25];

    MH_STATUS stIn = MH_CreateHook(
        processInputTarget,
        reinterpret_cast<void*>(&HookDolbyProcessInput),
        reinterpret_cast<void**>(&g_originalDolbyProcessInput));

    MH_STATUS stOut = MH_CreateHook(
        processOutputTarget,
        reinterpret_cast<void*>(&HookDolbyProcessOutput),
        reinterpret_cast<void**>(&g_originalDolbyProcessOutput));

    const bool createOk =
        (stIn == MH_OK || stIn == MH_ERROR_ALREADY_CREATED) &&
        (stOut == MH_OK || stOut == MH_ERROR_ALREADY_CREATED);

    if (!createOk) {
        Log(L"ERROR: Dolby proof MH_CreateHook input=%d output=%d",
            static_cast<int>(stIn),
            static_cast<int>(stOut));
        transform->Release();
        InterlockedExchange(&g_dolbyHookState, 0);
        return;
    }

    stIn = MH_EnableHook(processInputTarget);
    stOut = MH_EnableHook(processOutputTarget);
    transform->Release();

    const bool enableOk =
        (stIn == MH_OK || stIn == MH_ERROR_ENABLED) &&
        (stOut == MH_OK || stOut == MH_ERROR_ENABLED);

    if (!enableOk) {
        Log(L"ERROR: Dolby proof MH_EnableHook input=%d output=%d",
            static_cast<int>(stIn),
            static_cast<int>(stOut));
        InterlockedExchange(&g_dolbyHookState, 0);
        return;
    }

    InterlockedExchange(&g_dolbyHookState, 2);
    Log(L"FEL DOLBY INPUT+OUTPUT PROOF HOOKS ENABLED ProcessInput=%p ProcessOutput=%p",
        processInputTarget,
        processOutputTarget);
}


struct PendingPrimaryHevcInput {
    IMFTransform* decoder = nullptr;
    DWORD streamId = 0;
    IMFSample* sample = nullptr;
    DWORD flags = 0;
    LONGLONG sampleTime = -1;
};

static SRWLOCK g_primaryInputLock = SRWLOCK_INIT;
static std::deque<PendingPrimaryHevcInput> g_primaryInputQueue;
static IMFTransform* g_primaryHevcDecoder = nullptr;

static void ClearPrimaryInputQueue() {
    AcquireSRWLockExclusive(&g_primaryInputLock);
    for (auto& item : g_primaryInputQueue) {
        if (item.sample) item.sample->Release();
    }
    g_primaryInputQueue.clear();
    ReleaseSRWLockExclusive(&g_primaryInputLock);
}

static bool IsExactElReadyAtTime(LONGLONG sampleTime) {
    if (sampleTime < 0) return false;

    DecodedElFrame el;
    LONGLONG newest = -1;
    size_t queued = 0;
    if (!FindElFrameByTime(sampleTime, el, &newest, &queued)) return false;

    return el.subtype == MFVideoFormat_P010 &&
           el.width != 0 &&
           el.height != 0 &&
           !el.bytes.empty();
}

static void EnqueuePrimaryInput(
    IMFTransform* decoder,
    DWORD streamId,
    IMFSample* sample,
    DWORD flags) {

    if (!decoder || !sample) return;

    PendingPrimaryHevcInput item;
    item.decoder = decoder;
    item.streamId = streamId;
    item.sample = sample;
    item.flags = flags;
    sample->GetSampleTime(&item.sampleTime);
    sample->AddRef();

    AcquireSRWLockExclusive(&g_primaryInputLock);
    g_primaryInputQueue.push_back(item);
    const size_t q = g_primaryInputQueue.size();
    ReleaseSRWLockExclusive(&g_primaryInputLock);

    if (q <= 32) {
        Log(L"FEL BL INPUT HOLD time=%lld pending=%llu",
            static_cast<long long>(item.sampleTime),
            static_cast<unsigned long long>(q));
    }
}

static void PumpPrimaryInputs(IMFTransform* decoder) {
    if (!decoder || decoder != g_primaryHevcDecoder || !g_originalHevcProcessInput) return;

    constexpr LONGLONG kMissingElPastThreshold100ns = 900000; // 90 ms

    for (;;) {
        PendingPrimaryHevcInput item;
        bool have = false;
        bool exactEl = false;
        bool bypassMissingEl = false;
        LONGLONG newestElTime = -1;

        AcquireSRWLockShared(&g_primaryInputLock);
        if (!g_primaryInputQueue.empty()) {
            const auto& front = g_primaryInputQueue.front();

            if (front.decoder == decoder) {
                DecodedElFrame el;
                size_t queued = 0;
                exactEl = FindElFrameByTime(
                    front.sampleTime,
                    el,
                    &newestElTime,
                    &queued) &&
                    el.subtype == MFVideoFormat_P010 &&
                    el.width != 0 &&
                    el.height != 0 &&
                    !el.bytes.empty();

                if (!exactEl &&
                    front.sampleTime >= 0 &&
                    newestElTime >= 0 &&
                    newestElTime - front.sampleTime > kMissingElPastThreshold100ns) {
                    bypassMissingEl = true;
                }

                if (exactEl || bypassMissingEl) {
                    item = front;
                    if (item.sample) item.sample->AddRef();
                    have = true;
                }
            }
        }
        ReleaseSRWLockShared(&g_primaryInputLock);

        if (!have) break;

        HRESULT hr = g_originalHevcProcessInput(
            decoder,
            item.streamId,
            item.sample,
            item.flags);

        if (hr == MF_E_NOTACCEPTING) {
            if (item.sample) item.sample->Release();
            break;
        }

        if (FAILED(hr)) {
            Log(L"FEL BL INPUT RELEASE failed time=%lld hr=0x%08X",
                static_cast<long long>(item.sampleTime),
                static_cast<unsigned>(hr));
            if (item.sample) item.sample->Release();
            break;
        }

        AcquireSRWLockExclusive(&g_primaryInputLock);
        if (!g_primaryInputQueue.empty() &&
            g_primaryInputQueue.front().sample == item.sample) {

            auto front = g_primaryInputQueue.front();
            g_primaryInputQueue.pop_front();
            if (front.sample) front.sample->Release();
        }
        const size_t remaining = g_primaryInputQueue.size();
        ReleaseSRWLockExclusive(&g_primaryInputLock);

        if (exactEl) {
            Log(L"FEL BL INPUT RELEASE time=%lld pending=%llu exactEL=YES",
                static_cast<long long>(item.sampleTime),
                static_cast<unsigned long long>(remaining));
        } else {
            Log(L"FEL BL INPUT RELEASE time=%lld pending=%llu exactEL=NO reason=EL_PTS_MISSING newestEL=%lld",
                static_cast<long long>(item.sampleTime),
                static_cast<unsigned long long>(remaining),
                static_cast<long long>(newestElTime));
        }

        if (item.sample) item.sample->Release();
    }
}


static void ResetFelStateForSeek() {
    InterlockedExchange(&g_felContentActive, 0);
    ClearPrimaryInputQueue();

    AcquireSRWLockExclusive(&g_felLock);
    g_dvQueue.clear();
    ReleaseSRWLockExclusive(&g_felLock);

    AcquireSRWLockExclusive(&g_dolbyFelQueueLock);
    g_dolbyFelQueue.clear();
    g_lastDolbyFelAu = CapturedDvAccessUnit{};
    g_haveLastDolbyFelAu = false;
    ReleaseSRWLockExclusive(&g_dolbyFelQueueLock);

    AcquireSRWLockExclusive(&g_elDecoderLock);
    g_elFrames.clear();

    if (g_elDecoder) {
        const HRESULT flushHr =
            g_elDecoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);

        const HRESULT startHr =
            g_elDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        Log(L"FEL SEEK RESET ELDEC flushHr=0x%08X startHr=0x%08X",
            static_cast<unsigned>(flushHr),
            static_cast<unsigned>(startHr));
    }

    ReleaseSRWLockExclusive(&g_elDecoderLock);

    Log(L"FEL SEEK RESET queues cleared");
}

static HRESULT STDMETHODCALLTYPE HookHevcProcessMessage(
    IMFTransform* self,
    MFT_MESSAGE_TYPE message,
    ULONG_PTR param) {

    if (!g_originalHevcProcessMessage) return E_UNEXPECTED;

    if (self == g_primaryHevcDecoder &&
        message == MFT_MESSAGE_COMMAND_FLUSH) {

        Log(L"FEL PRIMARY FLUSH detected self=%p", self);
        ResetFelStateForSeek();
    }

    return g_originalHevcProcessMessage(self, message, param);
}

static HRESULT STDMETHODCALLTYPE HookHevcProcessInput(
    IMFTransform* self,
    DWORD streamId,
    IMFSample* sample,
    DWORD flags) {

    if (!g_originalHevcProcessInput) return E_UNEXPECTED;

    if (self == g_elDecoder) {
        return g_originalHevcProcessInput(self, streamId, sample, flags);
    }

    if (!sample || !g_felBridgeEnabled) {
        return g_originalHevcProcessInput(self, streamId, sample, flags);
    }

    const bool containsFel = CaptureDolbyUnits(sample);

    if (containsFel && g_primaryHevcDecoder != self) {
        ClearPrimaryInputQueue();
        g_primaryHevcDecoder = self;
        Log(L"FEL PRIMARY HEVC DETECTED self=%p capture=%ld",
            self,
            static_cast<long>(InterlockedCompareExchange(&g_felCaptureCount, 0, 0)));
    }

    if (self != g_primaryHevcDecoder || !containsFel) {
        return g_originalHevcProcessInput(self, streamId, sample, flags);
    }

    EnqueuePrimaryInput(self, streamId, sample, flags);
    PumpPrimaryInputs(self);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE HookHevcProcessOutput(
    IMFTransform* self,
    DWORD flags,
    DWORD outputBufferCount,
    MFT_OUTPUT_DATA_BUFFER* outputSamples,
    DWORD* status) {

    if (self == g_primaryHevcDecoder &&
        g_felBridgeEnabled &&
        InterlockedCompareExchange(&g_felContentActive, 0, 0) != 0) {
        PumpPrimaryInputs(self);
    }

    HRESULT hr = g_originalHevcProcessOutput
        ? g_originalHevcProcessOutput(self, flags, outputBufferCount, outputSamples, status)
        : E_UNEXPECTED;

    if (SUCCEEDED(hr) &&
        outputSamples &&
        self == g_primaryHevcDecoder &&
        InterlockedCompareExchange(&g_felContentActive, 0, 0) != 0) {

        for (DWORD i = 0; i < outputBufferCount; ++i) {
            if (outputSamples[i].pSample && g_felBridgeEnabled) {
                BridgeFel63ToOutput(outputSamples[i].pSample);
            }
        }
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookHevcSetInputType(
    IMFTransform* self, DWORD streamId, IMFMediaType* type, DWORD flags) {

    const bool mkvHevc = type && IsMatroskaHevc(type);
    bool p7Candidate = false;

    if (mkvHevc && (flags & MFT_SET_TYPE_TEST_ONLY) == 0) {
        ResetFelStateForSeek();
        g_primaryHevcDecoder = nullptr;
    }

    if (mkvHevc) {
        wchar_t existingProfile[32]{};
        const bool hasProfile = ReadRendererProfile(type, existingProfile, _countof(existingProfile));

        if (hasProfile) {
            p7Candidate = _wcsicmp(existingProfile, L"dvhe.07") == 0;
            Log(L"HEVC SetInputType MKV rendererProfile=%s action=%s",
                existingProfile, p7Candidate ? L"P7_CANDIDATE" : L"PASSTHROUGH_NATIVE_DV");
        } else {
            ForceP7Profile(type, L"HEVC SetInputType (MKV untagged P7 fallback)");
            p7Candidate = true;
            Log(L"HEVC SetInputType MKV rendererProfile=<none> action=P7_FALLBACK");
        }

        SetP7CandidateDecoder(self, p7Candidate);

        if (p7Candidate) {
            HRESULT naluHr = type->SetUINT32(kForwardCustomNalu, kDolbyVisionHevcNaluType);
            HRESULT orderHr = type->SetUINT32(kForwardCustomSeiDecodeOrder, TRUE);

            Log(L"FORCE MF_MT_FORWARD_CUSTOM_NALU=62 hr=0x%08X",
                static_cast<unsigned>(naluHr));
            Log(L"FORCE MF_DECODER_FWD_CUSTOM_SEI_DECODE_ORDER=1 hr=0x%08X",
                static_cast<unsigned>(orderHr));
        }
    }

    HRESULT hr = g_originalHevcSetInputType
        ? g_originalHevcSetInputType(self, streamId, type, flags)
        : E_UNEXPECTED;

    if (mkvHevc) {
        Log(L"HEVC SetInputType original returned hr=0x%08X flags=0x%08X self=%p p7Candidate=%s",
            static_cast<unsigned>(hr), flags, self, p7Candidate ? L"YES" : L"NO");

    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookHevcSetOutputType(
    IMFTransform* self, DWORD streamId, IMFMediaType* type, DWORD flags) {

    HRESULT hr = g_originalHevcSetOutputType
        ? g_originalHevcSetOutputType(self, streamId, type, flags)
        : E_UNEXPECTED;

    if (SUCCEEDED(hr) && type &&
        IsP7CandidateDecoder(self) &&
        IsP010OrNv12(type)) {

        wchar_t profile[32]{};
        UINT32 len = 0;
        HRESULT getHr = type->GetString(kRendererExtensionProfile, profile, _countof(profile), &len);
        if (FAILED(getHr) || _wcsicmp(profile, L"dvhe.07") != 0) {
            ForceP7Profile(type, L"POST HEVC SetOutputType (MKV)");
        } else {
            Log(L"HEVC output already carries dvhe.07");
        }
    }

    return hr;
}
static void InstallHevcDecoderHooks(IMFActivate* activate) {
    if (!activate) return;
    if (InterlockedCompareExchange(&g_decoderHookState, 1, 0) != 0) return;

    IMFTransform* transform = nullptr;
    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&transform));
    if (FAILED(hr) || !transform) {
        Log(L"HEVC probe ActivateObject failed hr=0x%08X", static_cast<unsigned>(hr));
        InterlockedExchange(&g_decoderHookState, 0);
        return;
    }

    void** vtbl = *reinterpret_cast<void***>(transform);
    void* setInputTarget = vtbl[15];
    void* setOutputTarget = vtbl[16];
    void* processMessageTarget = vtbl[23];
    void* processInputTarget = vtbl[24];
    void* processOutputTarget = vtbl[25];

    Log(L"HEVC decoder vtable SetInputType=%p SetOutputType=%p ProcessMessage=%p ProcessInput=%p ProcessOutput=%p",
        setInputTarget, setOutputTarget, processMessageTarget, processInputTarget, processOutputTarget);

    MH_STATUS stIn = MH_CreateHook(
        setInputTarget,
        reinterpret_cast<void*>(&HookHevcSetInputType),
        reinterpret_cast<void**>(&g_originalHevcSetInputType));

    if (stIn != MH_OK && stIn != MH_ERROR_ALREADY_CREATED) {
        Log(L"ERROR: HEVC SetInputType MH_CreateHook=%d (%S)", static_cast<int>(stIn), MH_StatusToString(stIn));
        transform->Release();
        InterlockedExchange(&g_decoderHookState, 0);
        return;
    }

    MH_STATUS stOut = MH_CreateHook(
        setOutputTarget,
        reinterpret_cast<void*>(&HookHevcSetOutputType),
        reinterpret_cast<void**>(&g_originalHevcSetOutputType));

    if (stOut != MH_OK && stOut != MH_ERROR_ALREADY_CREATED) {
        Log(L"ERROR: HEVC SetOutputType MH_CreateHook=%d (%S)", static_cast<int>(stOut), MH_StatusToString(stOut));
        transform->Release();
        InterlockedExchange(&g_decoderHookState, 0);
        return;
    }

    MH_STATUS stProcessMsg = MH_CreateHook(
        processMessageTarget,
        reinterpret_cast<void*>(&HookHevcProcessMessage),
        reinterpret_cast<void**>(&g_originalHevcProcessMessage));

    if (stProcessMsg != MH_OK && stProcessMsg != MH_ERROR_ALREADY_CREATED) {
        Log(L"ERROR: HEVC ProcessMessage MH_CreateHook=%d (%S)",
            static_cast<int>(stProcessMsg), MH_StatusToString(stProcessMsg));
        transform->Release();
        InterlockedExchange(&g_decoderHookState, 0);
        return;
    }

    MH_STATUS stProcessIn = MH_CreateHook(
        processInputTarget,
        reinterpret_cast<void*>(&HookHevcProcessInput),
        reinterpret_cast<void**>(&g_originalHevcProcessInput));

    if (stProcessIn != MH_OK && stProcessIn != MH_ERROR_ALREADY_CREATED) {
        Log(L"ERROR: HEVC ProcessInput MH_CreateHook=%d (%S)",
            static_cast<int>(stProcessIn), MH_StatusToString(stProcessIn));
        transform->Release();
        InterlockedExchange(&g_decoderHookState, 0);
        return;
    }

    MH_STATUS stProcessOut = MH_CreateHook(
        processOutputTarget,
        reinterpret_cast<void*>(&HookHevcProcessOutput),
        reinterpret_cast<void**>(&g_originalHevcProcessOutput));

    if (stProcessOut != MH_OK && stProcessOut != MH_ERROR_ALREADY_CREATED) {
        Log(L"ERROR: HEVC ProcessOutput MH_CreateHook=%d (%S)",
            static_cast<int>(stProcessOut), MH_StatusToString(stProcessOut));
        transform->Release();
        InterlockedExchange(&g_decoderHookState, 0);
        return;
    }

    stIn = MH_EnableHook(setInputTarget);
    stOut = MH_EnableHook(setOutputTarget);
    stProcessMsg = MH_EnableHook(processMessageTarget);
    stProcessIn = MH_EnableHook(processInputTarget);
    stProcessOut = MH_EnableHook(processOutputTarget);

    transform->Release();

    if ((stIn != MH_OK && stIn != MH_ERROR_ENABLED) ||
        (stOut != MH_OK && stOut != MH_ERROR_ENABLED) ||
        (stProcessMsg != MH_OK && stProcessMsg != MH_ERROR_ENABLED) ||
        (stProcessIn != MH_OK && stProcessIn != MH_ERROR_ENABLED) ||
        (stProcessOut != MH_OK && stProcessOut != MH_ERROR_ENABLED)) {

        Log(L"ERROR: HEVC hook enable setIn=%d setOut=%d processMsg=%d processIn=%d processOut=%d",
            static_cast<int>(stIn),
            static_cast<int>(stOut),
            static_cast<int>(stProcessMsg),
            static_cast<int>(stProcessIn),
            static_cast<int>(stProcessOut));

        InterlockedExchange(&g_decoderHookState, 0);
        return;
    }

    InterlockedExchange(&g_decoderHookState, 2);
    Log(L"HEVC + FEL BRIDGE + P010 EL + RPU/NLQ + DXGI PAIRING HOOKS ENABLED");
}

static HRESULT WINAPI HookMFTEnumEx(
    GUID category,
    UINT32 flags,
    const MFT_REGISTER_TYPE_INFO* input,
    const MFT_REGISTER_TYPE_INFO* output,
    IMFActivate*** activates,
    UINT32* count) {

    auto original = g_originalMFTEnumEx;
    if (!original) return E_UNEXPECTED;

    HRESULT hr = original(category, flags, input, output, activates, count);
    const LONG call = InterlockedIncrement(&g_hookCalls);

    bool p010Query = false;
    if (input && input->guidMajorType == MFMediaType_Video && input->guidSubtype == MFVideoFormat_P010) {
        p010Query = true;
    }

    if (call <= 20 || p010Query) {
        Log(L"INLINE MFTEnumEx call=%ld hr=0x%08X flags=0x%08X count=%u inputP010=%d",
            call, static_cast<unsigned>(hr), flags,
            (count ? *count : 0), p010Query ? 1 : 0);
    }

    if (FAILED(hr) || !activates || !count || !*activates) return hr;

    for (UINT32 i = 0; i < *count; ++i) {
        IMFActivate* a = (*activates)[i];
        if (!a) continue;

        if (IsDolbyRenderer(a)) {
            Log(L"INLINE found Dolby Vision Extensions at result=%u; enabling dvhe.07", i);
            SetProfilesWithP7(a);

            if (InterlockedCompareExchange(&g_dolbyHookState, 0, 0) == 0) {
                InstallDolbyProofHooks(a);
            }
        }

        if (IsHevcDecoder(a) && InterlockedCompareExchange(&g_decoderHookState, 0, 0) == 0) {
            Log(L"INLINE found HEVC decoder at result=%u; installing media-type hooks", i);
            InstallHevcDecoderHooks(a);
        }
    }

    return hr;
}

static DWORD WINAPI HookThread(void*) {
    Sleep(300);

    HMODULE mfplat = GetModuleHandleW(L"mfplat.dll");
    if (!mfplat) mfplat = LoadLibraryW(L"mfplat.dll");
    if (!mfplat) {
        Log(L"ERROR: mfplat.dll not loaded error=%lu", GetLastError());
        return 1;
    }

    void* target = reinterpret_cast<void*>(GetProcAddress(mfplat, "MFTEnumEx"));
    if (!target) {
        Log(L"ERROR: MFTEnumEx export not found");
        return 2;
    }

    wchar_t modePath[32768]{};
    wchar_t low6ModePath[32768]{};
    wchar_t splitModePath[32768]{};
    wchar_t lumaModePath[32768]{};
    g_felSplitEnabled = ReadFelSplitEnabled(splitModePath, _countof(splitModePath));
    g_felHighlightLimitEnabled = ReadFelLumaGuardEnabled(lumaModePath, _countof(lumaModePath));
    g_felBridgeEnabled = ReadFelBridgeEnabled(modePath, _countof(modePath));
    g_low6CarrierEnabled = ReadLow6CarrierEnabled(low6ModePath, _countof(low6ModePath));

    Log(L"Version 1.0-beta HDR10 LEFT / Dolby Vision FEL RIGHT; NO L5 MASK + profile isolation + seek/flush reset MFTEnumEx=%p", target);
    Log(L"FEL BRIDGE MODE=%s modeFile=%s",
        g_felBridgeEnabled ? L"ON" : L"OFF",
        modePath);
    Log(L"FEL LOW6 MODE=%s modeFile=%s",
        g_low6CarrierEnabled ? L"CARRIER" : L"MASK10",
        low6ModePath);
    Log(L"FEL SPLIT MODE=%s modeFile=%s",
        g_felSplitEnabled == 2 ? L"DIFF" : (g_felSplitEnabled ? L"ON" : L"OFF"),
        splitModePath);
    Log(L"FEL LUMA GUARD MODE=%s modeFile=%s",
        g_felHighlightLimitEnabled ? L"DOLBY_LUMA_ANCHORED" : L"RAW",
        lumaModePath);
    Log(L"FEL L5 SOFTWARE MASK=OFF");

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log(L"ERROR: MH_Initialize=%d (%S)", static_cast<int>(st), MH_StatusToString(st));
        return 3;
    }

    st = MH_CreateHook(target, reinterpret_cast<void*>(&HookMFTEnumEx),
        reinterpret_cast<void**>(&g_originalMFTEnumEx));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        Log(L"ERROR: MH_CreateHook=%d (%S)", static_cast<int>(st), MH_StatusToString(st));
        return 4;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        Log(L"ERROR: MH_EnableHook=%d (%S)", static_cast<int>(st), MH_StatusToString(st));
        return 5;
    }

    Log(L"INLINE HOOK ENABLED original trampoline=%p", reinterpret_cast<void*>(g_originalMFTEnumEx));
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
