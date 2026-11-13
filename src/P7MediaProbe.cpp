#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include "DvWindowsPreflight.h"

namespace {
struct MediaCandidate {
    std::wstring path;
    HANDLE handle = nullptr;
};

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool GetMediaPath(HANDLE file, std::wstring& path) {
    if (GetFileType(file) != FILE_TYPE_DISK) return false;
    wchar_t name[32768]{};
    DWORD count = GetFinalPathNameByHandleW(file, name, _countof(name), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (count == 0 || count >= _countof(name)) {
        count = GetFinalPathNameByHandleW(file, name, _countof(name), FILE_NAME_OPENED | VOLUME_NAME_DOS);
    }
    if (count == 0 || count >= _countof(name)) return false;
    path.assign(name, count);
    return dvpreflight::IsMediaPath(path);
}

std::vector<unsigned char> QueryHandles() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return {};
    using NtQuerySystemInformationFn = LONG (NTAPI*)(ULONG, void*, ULONG, ULONG*);
    auto query = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!query) return {};
    std::vector<unsigned char> bytes(1024 * 1024);
    for (int i = 0; i < 7; ++i) {
        ULONG sizeNeeded = 0;
        const LONG status = query(64, bytes.data(), static_cast<ULONG>(bytes.size()), &sizeNeeded);
        if (status >= 0) return bytes;
        if (status != static_cast<LONG>(0xC0000004)) return {};
        const std::size_t larger = std::max(bytes.size() * 2, static_cast<std::size_t>(sizeNeeded) + 4096);
        if (larger > 64 * 1024 * 1024) return {};
        bytes.resize(larger);
    }
    return {};
}

std::vector<MediaCandidate> FindProcessMediaHandles(DWORD pid, const std::vector<unsigned char>& tableBytes) {
    std::vector<MediaCandidate> found;
    if (tableBytes.size() < offsetof(dvpreflight::HandleTable, entries)) return found;
    HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) return found;
    const auto* table = reinterpret_cast<const dvpreflight::HandleTable*>(tableBytes.data());
    const std::size_t maxCount = (tableBytes.size() - offsetof(dvpreflight::HandleTable, entries)) /
                                 sizeof(dvpreflight::HandleEntry);
    const std::size_t count = std::min(static_cast<std::size_t>(table->count), maxCount);
    std::size_t scanned = 0;
    for (std::size_t i = 0; i < count && scanned < 4096; ++i) {
        const auto& entry = table->entries[i];
        if (entry.processId != static_cast<ULONG_PTR>(pid)) continue;
        ++scanned;
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(process, reinterpret_cast<HANDLE>(entry.handleValue), GetCurrentProcess(),
                             &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
        std::wstring path;
        if (!GetMediaPath(duplicate, path)) {
            CloseHandle(duplicate);
            continue;
        }
        const bool alreadySeen = std::any_of(found.begin(), found.end(), [&](const MediaCandidate& existing) {
            return SamePath(existing.path, path);
        });
        if (alreadySeen) {
            CloseHandle(duplicate);
        } else {
            found.push_back({path, duplicate});
        }
        if (found.size() > 5) break;
    }
    CloseHandle(process);
    return found;
}

std::vector<DWORD> FindRelatedMediaBrokerPids(DWORD pid) {
    std::vector<DWORD> related;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return related;
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    DWORD parent = 0;
    std::vector<PROCESSENTRY32W> processes;
    if (Process32FirstW(snapshot, &process)) {
        do {
            processes.push_back(process);
            if (process.th32ProcessID == pid) parent = process.th32ParentProcessID;
            process.dwSize = sizeof(process);
        } while (Process32NextW(snapshot, &process));
    }
    CloseHandle(snapshot);
    DWORD targetSession = 0;
    if (!ProcessIdToSessionId(pid, &targetSession)) return related;
    for (const auto& entry : processes) {
        const DWORD candidate = entry.th32ProcessID;
        if (candidate == pid || candidate == 0) continue;
        if (candidate != parent && entry.th32ParentProcessID != pid) continue;
        const wchar_t* executable = entry.szExeFile;
        if (_wcsicmp(executable, L"RuntimeBroker.exe") != 0 &&
            _wcsicmp(executable, L"dllhost.exe") != 0 &&
            _wcsicmp(executable, L"mfpmp.exe") != 0 &&
            _wcsicmp(executable, L"ApplicationFrameHost.exe") != 0) continue;
        DWORD session = 0;
        if (ProcessIdToSessionId(candidate, &session) && session == targetSession) {
            related.push_back(candidate);
        }
        if (related.size() >= 8) break;
    }
    return related;
}

void CloseCandidates(std::vector<MediaCandidate>& candidates) {
    for (auto& candidate : candidates) if (candidate.handle) CloseHandle(candidate.handle);
}

bool InspectPath(const std::wstring& path, dvprobe::Result& result) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = dvpreflight::InspectFileHandle(file, result);
    CloseHandle(file);
    return ok;
}

bool RecentShortcutCandidates(std::vector<std::wstring>& paths, ULONGLONG processStarted) {
    PWSTR recentFolder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &recentFolder))) return false;
    std::wstring search(recentFolder);
    CoTaskMemFree(recentFolder);
    search += L"\\*.lnk";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG nowValue = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const ULONGLONG ageLimit = 4ULL * 10000000ULL;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG modified = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                                   fd.ftLastWriteTime.dwLowDateTime;
        if (nowValue < modified || nowValue - modified > ageLimit) continue;
        const ULONGLONG earlyWindow = 2ULL * 10000000ULL;
        const ULONGLONG lateWindow = 2ULL * 10000000ULL;
        if (modified + earlyWindow < processStarted || modified > processStarted + lateWindow) continue;
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&link)))) continue;
        IPersistFile* persist = nullptr;
        const HRESULT qhr = link->QueryInterface(IID_PPV_ARGS(&persist));
        if (SUCCEEDED(qhr)) {
            std::wstring shortcut = search.substr(0, search.size() - 5) + fd.cFileName;
            if (SUCCEEDED(persist->Load(shortcut.c_str(), STGM_READ))) {
                wchar_t target[32768]{};
                WIN32_FIND_DATAW unused{};
                if (SUCCEEDED(link->GetPath(target, _countof(target), &unused, SLGP_RAWPATH)) &&
                    target[0] && dvpreflight::IsMediaPath(target) &&
                    GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
                    const bool alreadySeen = std::any_of(paths.begin(), paths.end(), [&](const auto& known) {
                        return SamePath(known, target);
                    });
                    if (!alreadySeen) paths.emplace_back(target);
                }
            }
            persist->Release();
        }
        link->Release();
        if (paths.size() > 4) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return !paths.empty();
}

bool WriteDecision(const std::wstring& dest, const wchar_t* decision) {
    const std::wstring temp = dest + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD count = 0;
    const DWORD bytes = static_cast<DWORD>(wcslen(decision) * sizeof(wchar_t));
    const bool ok = WriteFile(f, decision, bytes, &count, nullptr) && count == bytes;
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 3;
    const DWORD pid = wcstoul(argv[1], nullptr, 10);
    if (!pid || pid == GetCurrentProcessId()) return 3;
    const std::wstring dest(argv[2]);
    HANDLE targetProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!targetProcess) return 2;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool validCreation = GetProcessTimes(targetProcess, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(targetProcess);
    if (!validCreation) return 2;
    const ULONGLONG startTime = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) |
                                created.dwLowDateTime;

    dvprobe::Result stream{};
    bool inspected = false;
    std::wstring chosenPath;
    std::wstring provenance;
    const auto handles = QueryHandles();
    auto media = FindProcessMediaHandles(pid, handles);
    if (media.size() == 1) {
        inspected = dvpreflight::InspectFileHandle(media.front().handle, stream);
        if (inspected) {
            chosenPath = media.front().path;
            provenance = L"DIRECT";
        }
    }
    const bool multipleDirect = media.size() > 1;
    CloseCandidates(media);

    if (!inspected && !multipleDirect && !handles.empty()) {
        std::vector<MediaCandidate> brokerFiles;
        for (const DWORD brokerPid : FindRelatedMediaBrokerPids(pid)) {
            auto broker = FindProcessMediaHandles(brokerPid, handles);
            for (auto& file : broker) {
                const bool duplicate = std::any_of(brokerFiles.begin(), brokerFiles.end(),
                    [&](const MediaCandidate& known) { return SamePath(known.path, file.path); });
                if (duplicate) {
                    CloseHandle(file.handle);
                } else {
                    brokerFiles.push_back(file);
                }
            }
        }
        if (brokerFiles.size() == 1) {
            inspected = dvpreflight::InspectFileHandle(brokerFiles.front().handle, stream);
            if (inspected) {
                chosenPath = brokerFiles.front().path;
                provenance = L"BROKER";
            }
        }
        CloseCandidates(brokerFiles);
    }

    if (!inspected && !multipleDirect) {
        const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::vector<std::wstring> recent;
        if (SUCCEEDED(initHr)) {
            RecentShortcutCandidates(recent, startTime);
            CoUninitialize();
        }
        if (recent.size() == 1) {
            inspected = InspectPath(recent.front(), stream);
            if (inspected) {
                chosenPath = recent.front();
                provenance = L"RECENT";
            }
        }
    }

    if (!inspected || chosenPath.empty() || (stream.dolbyVision &&
        stream.profile != 5 && stream.profile != 7 &&
        stream.profile != 8 && stream.profile != 9)) {
        DeleteFileW(dest.c_str());
        return 2;
    }
    const wchar_t* status = stream.dolbyVision ? L"DV" : L"HEVC";
    const std::wstring record = std::wstring(status) + L"|" + std::to_wstring(pid) + L"|" +
        std::to_wstring(startTime) + L"|" + provenance + L"|" +
        std::to_wstring(stream.profile) + L"|" +
        std::to_wstring(stream.configurationRecords) + L"|" +
        std::to_wstring(stream.rpuUnits) + L"|" +
        std::to_wstring(stream.enhancementUnits) + L"|" + chosenPath;
    if (!WriteDecision(dest, record.c_str())) return 3;
    return stream.dolbyVision ? 0 : 1;
}
