#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>
#include <vector>
#include <cwchar>
#include <cstdint>

static DWORD FindProcess(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return pid;
}

static std::wstring BaseName(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

static uintptr_t FindRemoteModuleBase(DWORD pid, const std::wstring& moduleName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    uintptr_t base = 0;

    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) {
                base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }
        } while (Module32NextW(snap, &me));
    }

    CloseHandle(snap);
    return base;
}

static uintptr_t ResolveRemoteProc(DWORD pid, FARPROC proc, std::wstring& ownerModule) {
    HMODULE localModule = nullptr;
    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(proc), &localModule)) {
        return 0;
    }

    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(localModule, modulePath, MAX_PATH)) return 0;
    ownerModule = BaseName(modulePath);

    const uintptr_t localBase = reinterpret_cast<uintptr_t>(localModule);
    const uintptr_t localProc = reinterpret_cast<uintptr_t>(proc);
    const uintptr_t rva = localProc - localBase;
    uintptr_t remoteBase = 0;
    for (int attempt = 0; attempt < 12 && !remoteBase; ++attempt) {
        remoteBase = FindRemoteModuleBase(pid, ownerModule);
        if (!remoteBase) Sleep(25);
    }
    if (!remoteBase) return 0;

    return remoteBase + rva;
}

static bool RemoteHasModule(DWORD pid, const std::wstring& dllPath) {
    const std::wstring wanted = BaseName(dllPath);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, wanted.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static void PrintMitigations(HANDLE process) {
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig{};
    if (GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &sig, sizeof(sig))) {
        std::wcout << L"SignaturePolicy: MicrosoftSignedOnly=" << sig.MicrosoftSignedOnly
                   << L" StoreSignedOnly=" << sig.StoreSignedOnly
                   << L" MitigationOptIn=" << sig.MitigationOptIn << L"\n";
    }

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn{};
    if (GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &dyn, sizeof(dyn))) {
        std::wcout << L"DynamicCodePolicy: ProhibitDynamicCode=" << dyn.ProhibitDynamicCode
                   << L" AllowThreadOptOut=" << dyn.AllowThreadOptOut << L"\n";
    }

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY img{};
    if (GetProcessMitigationPolicy(process, ProcessImageLoadPolicy, &img, sizeof(img))) {
        std::wcout << L"ImageLoadPolicy: NoRemoteImages=" << img.NoRemoteImages
                   << L" NoLowMandatoryLabelImages=" << img.NoLowMandatoryLabelImages
                   << L" PreferSystem32Images=" << img.PreferSystem32Images << L"\n";
    }
}

struct RemoteContext {
    ULONGLONG loadLibraryW;
    ULONGLONG getLastError;
    ULONGLONG moduleResult;
    DWORD lastError;
    DWORD reserved;
    wchar_t path[1024];
};

static bool DiagnosticLoad(HANDLE process, DWORD pid, const std::wstring& dllPath, DWORD& loaderError) {
    FARPROC localLoad = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    FARPROC localErr = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLastError");
    if (!localLoad || !localErr) return false;

    std::wstring loadOwner, errOwner;
    uintptr_t remoteLoad = ResolveRemoteProc(pid, localLoad, loadOwner);
    uintptr_t remoteErr = ResolveRemoteProc(pid, localErr, errOwner);
    if (!remoteLoad || !remoteErr) {
        std::wcerr << L"Could not resolve remote loader APIs. LoadLibrary owner=" << loadOwner
                   << L" GetLastError owner=" << errOwner << L"\n";
        return false;
    }

    RemoteContext ctx{};
    ctx.loadLibraryW = static_cast<ULONGLONG>(remoteLoad);
    ctx.getLastError = static_cast<ULONGLONG>(remoteErr);
    wcsncpy_s(ctx.path, dllPath.c_str(), _TRUNCATE);

    void* remoteCtx = VirtualAllocEx(process, nullptr, sizeof(ctx), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteCtx) {
        std::wcerr << L"VirtualAllocEx(ctx) failed: " << GetLastError() << L"\n";
        return false;
    }

    const unsigned char stub[] = {
        0x53,
        0x48,0x89,0xCB,
        0x48,0x83,0xEC,0x20,
        0x48,0x8B,0x03,
        0x48,0x8D,0x4B,0x20,
        0xFF,0xD0,
        0x48,0x89,0x43,0x10,
        0x48,0x85,0xC0,
        0x75,0x09,
        0x48,0x8B,0x43,0x08,
        0xFF,0xD0,
        0x89,0x43,0x18,
        0x33,0xC0,
        0x48,0x83,0xC4,0x20,
        0x5B,
        0xC3
    };

    void* remoteCode = VirtualAllocEx(process, nullptr, sizeof(stub), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteCode) {
        std::wcerr << L"VirtualAllocEx(code) failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteCtx, 0, MEM_RELEASE);
        return false;
    }

    SIZE_T written = 0;
    bool ok = WriteProcessMemory(process, remoteCtx, &ctx, sizeof(ctx), &written) != FALSE;
    ok = ok && WriteProcessMemory(process, remoteCode, stub, sizeof(stub), &written) != FALSE;
    if (!ok) {
        std::wcerr << L"WriteProcessMemory diagnostic stub failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
        VirtualFreeEx(process, remoteCtx, 0, MEM_RELEASE);
        return false;
    }

    FlushInstructionCache(process, remoteCode, sizeof(stub));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteCode), remoteCtx, 0, nullptr);
    if (!thread) {
        std::wcerr << L"CreateRemoteThread(stub) failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
        VirtualFreeEx(process, remoteCtx, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);

    RemoteContext result{};
    if (!ReadProcessMemory(process, remoteCtx, &result, sizeof(result), nullptr)) {
        std::wcerr << L"ReadProcessMemory(ctx) failed: " << GetLastError() << L"\n";
    }

    loaderError = result.lastError;
    const bool loaded = result.moduleResult != 0 || RemoteHasModule(pid, dllPath);

    VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
    VirtualFreeEx(process, remoteCtx, 0, MEM_RELEASE);
    return loaded;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"Usage: P7Injector.exe <full path to P7ProfileHook.dll> [Microsoft.Media.Player.exe|Video.UI.exe|Microsoft.Photos.exe|Photos.exe]\n";
        return 2;
    }

    wchar_t fullDll[32768]{};
    if (!GetFullPathNameW(argv[1], _countof(fullDll), fullDll, nullptr)) {
        std::wcerr << L"GetFullPathName failed: " << GetLastError() << L"\n";
        return 3;
    }

    if (GetFileAttributesW(fullDll) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"DLL not found: " << fullDll << L"\n";
        return 4;
    }

    const wchar_t* target = L"Microsoft.Media.Player.exe";
    if (argc >= 3) {
        if (_wcsicmp(argv[2], L"Microsoft.Media.Player.exe") == 0 ||
            _wcsicmp(argv[2], L"Video.UI.exe") == 0 ||
            _wcsicmp(argv[2], L"Microsoft.Photos.exe") == 0 ||
            _wcsicmp(argv[2], L"Photos.exe") == 0) {
            target = argv[2];
        } else {
            std::wcerr << L"Unsupported player executable.\n";
            return 2;
        }
    }
    DWORD pid = 0;
    if (argc >= 4) {
        wchar_t* end = nullptr;
        unsigned long requested = wcstoul(argv[3], &end, 10);
        if (!requested || !end || *end != L'\0') {
            std::wcerr << L"Invalid target PID.\n";
            return 2;
        }
        pid = static_cast<DWORD>(requested);
    } else {
        pid = FindProcess(target);
    }
    if (!pid) {
        std::wcerr << L"Target player not found. Open the player before injection.\n";
        return 5;
    }

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);

    if (!process) {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return 6;
    }

    wchar_t imagePath[32768]{};
    DWORD imageChars = _countof(imagePath);
    if (QueryFullProcessImageNameW(process, 0, imagePath, &imageChars)) {
        std::wcout << L"Target: " << imagePath << L"\n";
    }
    if (argc >= 4) {
        std::wstring current = BaseName(imagePath);
        if (imagePath[0] == L'\0' || _wcsicmp(current.c_str(), target) != 0) {
            std::wcerr << L"PID does not belong to the requested player.\n";
            CloseHandle(process);
            return 7;
        }
    }
    std::wcout << L"PID: " << pid << L"\n";
    PrintMitigations(process);

    DWORD loaderError = 0;
    const bool loaded = DiagnosticLoad(process, pid, fullDll, loaderError);
    CloseHandle(process);

    if (!loaded) {
        std::wcerr << L"LoadLibraryW failed inside Media Player. Win32 error=" << loaderError;
        if (loaderError) {
            wchar_t* text = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, loaderError, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
            if (text) {
                std::wcerr << L" (" << text << L")";
                LocalFree(text);
            }
        }
        std::wcerr << L"\n";
        return 11;
    }

    std::wcout << L"INJECTED into " << target << L" PID " << pid << L"\n";
    std::wcout << L"The hook log is next to the injected DLL.\n";
    return 0;
}
