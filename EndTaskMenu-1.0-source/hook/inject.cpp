// EndTaskInject.exe - injects EndTaskHook.dll into ShellExperienceHost.exe.
// Must run elevated (the shell process is not, but SeDebugPrivilege and
// PROCESS_VM_WRITE into a system app require admin).
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <vector>

static bool EnableDebugPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(tok);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    const BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(tok);
    return ok && GetLastError() == ERROR_SUCCESS;
}

static std::vector<DWORD> FindProcesses(const wchar_t* exeName) {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, exeName)) out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

static bool ModuleLoaded(DWORD pid, const wchar_t* moduleName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (!_wcsicmp(me.szModule, moduleName)) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static bool Inject(DWORD pid, const std::wstring& dllPath) {
    printf("[*] pid %lu: opening process\n", pid);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        printf("[x] OpenProcess failed: %lu\n", GetLastError());
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        printf("[x] VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        printf("[x] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel, "LoadLibraryW"));
    if (!loadLibrary) {
        printf("[x] LoadLibraryW address not found\n");
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        printf("[x] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);

    if (!exitCode) {
        printf("[x] remote LoadLibrary returned null (error %lu)\n", GetLastError());
        CloseHandle(proc);
        return false;
    }

    const bool loaded = ModuleLoaded(pid, L"EndTaskHook.dll");
    printf("[+] injected, remote module base 0x%llX, module list=%s\n",
           (unsigned long long)exitCode, loaded ? "yes" : "no");
    CloseHandle(proc);
    return loaded;
}

int wmain(int argc, wchar_t** argv) {
    if (!EnableDebugPrivilege())
        printf("[!] SeDebugPrivilege not enabled (error %lu), continuing\n", GetLastError());

    wchar_t dll[MAX_PATH * 2] = {};
    if (argc >= 2) {
        wcscpy_s(dll, argv[1]);
    } else {
        GetModuleFileNameW(nullptr, dll, MAX_PATH * 2);
        wchar_t* slash = wcsrchr(dll, L'\\');
        if (slash) *(slash + 1) = 0;
        wcscat_s(dll, L"EndTaskHook.dll");
    }

    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        printf("[x] DLL not found: %ls\n", dll);
        return 1;
    }

    printf("[*] DLL: %ls\n", dll);

    const wchar_t* baseName = wcsrchr(dll, L'\\');
    baseName = baseName ? baseName + 1 : dll;

    auto pids = FindProcesses(L"ShellExperienceHost.exe");
    if (pids.empty()) {
        printf("[x] ShellExperienceHost.exe not running\n");
        return 1;
    }

    int ok = 0;
    for (DWORD pid : pids) {
        if (ModuleLoaded(pid, baseName)) {
            printf("[=] pid %lu already has %ls\n", pid, baseName);
            ++ok;
            continue;
        }
        if (Inject(pid, dll)) ++ok;
    }

    printf("[*] done: %d/%zu process(es)\n", ok, pids.size());
    return ok == (int)pids.size() ? 0 : 1;
}
