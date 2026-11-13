#pragma once

#include "DvPreflightScanner.h"
#include <windows.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>

namespace dvpreflight {

struct Result {
    bool foundFile = false;
    bool inspected = false;
    bool ambiguous = false;
    dvprobe::Result stream{};
    std::wstring path;
};

struct HandleEntry {
    void* object;
    ULONG_PTR processId;
    ULONG_PTR handleValue;
    ULONG grantedAccess;
    USHORT creatorBackTraceIndex;
    USHORT objectTypeIndex;
    ULONG handleAttributes;
    ULONG reserved;
};

struct HandleTable {
    ULONG_PTR count;
    ULONG_PTR reserved;
    HandleEntry entries[1];
};

inline bool IsMediaPath(const std::wstring& path) {
    const wchar_t* dot = wcsrchr(path.c_str(), L'.');
    if (!dot) return false;
    return _wcsicmp(dot, L".mkv") == 0 ||
           _wcsicmp(dot, L".mp4") == 0 ||
           _wcsicmp(dot, L".m4v") == 0 ||
           _wcsicmp(dot, L".mov") == 0 ||
           _wcsicmp(dot, L".m2ts") == 0 ||
           _wcsicmp(dot, L".ts") == 0;
}

inline bool ResolveRpuProfile(dvprobe::Result& stream) {
    if (!stream.dolbyVision) return false;
    if (stream.profile == 5 || stream.profile == 7 || stream.profile == 8 || stream.profile == 9) return true;
    if (stream.firstRpu.empty()) return false;
    wchar_t modulePath[32768]{};
    HMODULE containing = nullptr;
    HMODULE library = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ResolveRpuProfile), &containing) &&
        GetModuleFileNameW(containing, modulePath, _countof(modulePath))) {
        wchar_t* end = wcsrchr(modulePath, L'\\');
        if (end) {
            end[1] = 0;
            wcscat_s(modulePath, L"dovi.dll");
            library = LoadLibraryW(modulePath);
        }
    }
    if (!library) library = LoadLibraryW(L"C:\\ProgramData\\MillaAV\\P7DV\\dovi.dll");
    if (!library) return false;
    using Parse = void* (__cdecl*)(const std::uint8_t*, std::size_t);
    using GetHeader = const void* (__cdecl*)(const void*);
    using Free = void (__cdecl*)(void*);
    using FreeHeader = void (__cdecl*)(const void*);
    const auto parse = reinterpret_cast<Parse>(GetProcAddress(library, "dovi_parse_unspec62_nalu"));
    const auto header = reinterpret_cast<GetHeader>(GetProcAddress(library, "dovi_rpu_get_header"));
    const auto release = reinterpret_cast<Free>(GetProcAddress(library, "dovi_rpu_free"));
    const auto releaseHeader = reinterpret_cast<FreeHeader>(GetProcAddress(library, "dovi_rpu_free_header"));
    unsigned parsedProfile = 0;
    if (parse && header && release && releaseHeader) {
        void* rpu = parse(stream.firstRpu.data(), stream.firstRpu.size());
        if (rpu) {
            const auto* h = static_cast<const unsigned char*>(header(rpu));
            if (h) {
                parsedProfile = h[0];
                releaseHeader(h);
            }
            release(rpu);
        }
    }
    FreeLibrary(library);
    if (parsedProfile == 5 || parsedProfile == 7 || parsedProfile == 8 || parsedProfile == 9) {
        stream.profile = parsedProfile;
        return true;
    }
    return false;
}

inline bool InspectFileHandle(HANDLE file, dvprobe::Result& result) {
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 12) return false;
    const LONGLONG maxScan = 32LL * 1024 * 1024;
    const SIZE_T length = static_cast<SIZE_T>(std::min(fileSize.QuadPart, maxScan));

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) return false;
    const unsigned char* view = static_cast<const unsigned char*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, length));
    if (!view) {
        CloseHandle(mapping);
        return false;
    }

    result = dvprobe::Inspect(view, length);
    ResolveRpuProfile(result);
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return true;
}

struct RecentFile {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE handle = nullptr;
    std::wstring path;
    ULONGLONG openedAt = 0;
};

inline RecentFile& RecentlyOpened() {
    static RecentFile recent{};
    return recent;
}

inline void RememberFile(HANDLE file, const wchar_t* path) {
    if (!path || file == INVALID_HANDLE_VALUE || !file) return;
    const std::wstring name(path);
    if (!IsMediaPath(name)) return;
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &copy,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    RecentFile& recent = RecentlyOpened();
    AcquireSRWLockExclusive(&recent.lock);
    HANDLE old = recent.handle;
    recent.handle = copy;
    recent.path = name;
    recent.openedAt = GetTickCount64();
    ReleaseSRWLockExclusive(&recent.lock);
    if (old) CloseHandle(old);
}

inline Result InspectRememberedFile() {
    Result result{};
    RecentFile& recent = RecentlyOpened();
    HANDLE file = nullptr;
    AcquireSRWLockShared(&recent.lock);
    const ULONGLONG now = GetTickCount64();
    if (recent.handle && now >= recent.openedAt && now - recent.openedAt < 20000) {
        result.path = recent.path;
        DuplicateHandle(GetCurrentProcess(), recent.handle, GetCurrentProcess(), &file,
                        0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    ReleaseSRWLockShared(&recent.lock);
    if (!file) return result;
    result.foundFile = true;
    result.inspected = InspectFileHandle(file, result.stream);
    CloseHandle(file);
    return result;
}

inline Result InspectActiveMediaFile() {
    Result result{};
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return result;
    using NtQuerySystemInformationFn = LONG (NTAPI*)(ULONG, void*, ULONG, ULONG*);
    auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!query) return result;

    std::vector<unsigned char> buffer(1024 * 1024);
    LONG status = -1;
    ULONG required = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
        status = query(64, buffer.data(), static_cast<ULONG>(buffer.size()), &required);
        if (status >= 0) break;
        if (status != static_cast<LONG>(0xC0000004)) return result;
        const std::size_t nextSize = std::max(buffer.size() * 2, static_cast<std::size_t>(required) + 4096);
        if (nextSize > 32 * 1024 * 1024) return result;
        buffer.resize(nextSize);
    }
    if (status < 0) return result;

    const auto* table = reinterpret_cast<const HandleTable*>(buffer.data());
    const std::size_t maxEntries = (buffer.size() - offsetof(HandleTable, entries)) / sizeof(HandleEntry);
    const std::size_t count = std::min(static_cast<std::size_t>(table->count), maxEntries);
    const ULONG_PTR selfPid = static_cast<ULONG_PTR>(GetCurrentProcessId());

    struct Candidate { std::wstring path; HANDLE file; };
    std::vector<Candidate> candidates;
    std::vector<std::wstring> uniquePaths;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& entry = table->entries[i];
        if (entry.processId != selfPid) continue;

        HANDLE copy = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(entry.handleValue),
                             GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
        if (GetFileType(copy) != FILE_TYPE_DISK) {
            CloseHandle(copy);
            continue;
        }

        wchar_t filename[32768]{};
        DWORD used = GetFinalPathNameByHandleW(copy, filename, _countof(filename),
                                               FILE_NAME_OPENED | VOLUME_NAME_DOS);
        if (used == 0 || used >= _countof(filename)) {
            used = GetFinalPathNameByHandleW(copy, filename, _countof(filename),
                                             FILE_NAME_OPENED | VOLUME_NAME_NT);
        }
        if (used == 0 || used >= _countof(filename)) {
            CloseHandle(copy);
            continue;
        }
        const std::wstring path(filename);
        if (!IsMediaPath(path)) {
            CloseHandle(copy);
            continue;
        }

        bool duplicatePath = false;
        for (const auto& known : uniquePaths) {
            if (_wcsicmp(known.c_str(), path.c_str()) == 0) {
                duplicatePath = true;
                break;
            }
        }
        if (duplicatePath) {
            CloseHandle(copy);
            continue;
        }
        uniquePaths.push_back(path);
        candidates.push_back({path, copy});
    }

    if (!candidates.empty()) {
        result.foundFile = true;
        result.ambiguous = candidates.size() > 1;
    }

    if (candidates.size() == 1) {
        result.path = candidates[0].path;
        LARGE_INTEGER fileSize{};
        FILETIME writeTime{};
        const bool haveIdentity =
            GetFileSizeEx(candidates[0].file, &fileSize) &&
            GetFileTime(candidates[0].file, nullptr, nullptr, &writeTime);
        struct CachedResult {
            LONGLONG size;
            FILETIME modified;
            dvprobe::Result stream;
        };
        static SRWLOCK cacheLock = SRWLOCK_INIT;
        static std::unordered_map<std::wstring, CachedResult> cache;

        if (haveIdentity) {
            AcquireSRWLockShared(&cacheLock);
            const auto it = cache.find(result.path);
            if (it != cache.end() && it->second.size == fileSize.QuadPart &&
                CompareFileTime(&it->second.modified, &writeTime) == 0) {
                result.stream = it->second.stream;
                result.inspected = true;
            }
            ReleaseSRWLockShared(&cacheLock);
        }
        if (!result.inspected) {
            result.inspected = InspectFileHandle(candidates[0].file, result.stream);
            if (!result.inspected) {
                HANDLE alternate = CreateFileW(result.path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (alternate != INVALID_HANDLE_VALUE) {
                    result.inspected = InspectFileHandle(alternate, result.stream);
                    CloseHandle(alternate);
                }
            }
            if (result.inspected && haveIdentity) {
                AcquireSRWLockExclusive(&cacheLock);
                if (cache.size() >= 64) cache.clear();
                cache[result.path] = {fileSize.QuadPart, writeTime, result.stream};
                ReleaseSRWLockExclusive(&cacheLock);
            }
        }
    }

    for (const auto& candidate : candidates) CloseHandle(candidate.file);
    return result;
}

}
