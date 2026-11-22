// EndTaskHelper.exe - elevated background helper.
//  - keeps EndTaskHook.dll injected into ShellExperienceHost.exe
//  - watches request.txt written by the injected row and kills the target app
//
// Installed at C:\ProgramData\EndTaskMenu and launched at logon by the
// "EndTaskMenu" scheduled task (highest privileges).
#include <windows.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>

#include "target.h"
#include "util.h"

static const wchar_t* kDir  = L"C:\\ProgramData\\EndTaskMenu";
static const wchar_t* kDll  = L"C:\\ProgramData\\EndTaskMenu\\EndTaskHook.dll";
static const wchar_t* kReq  = L"C:\\ProgramData\\EndTaskMenu\\request.txt";
static const wchar_t* kLog  = L"C:\\ProgramData\\EndTaskMenu\\helper.log";

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);

    HANDLE f = CreateFileW(kLog, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2300];
    int n = swprintf_s(line, L"[%02d:%02d:%02d.%03d][%lu] %s\r\n",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       GetCurrentThreadId(), buf);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(f, line, (DWORD)(n * sizeof(wchar_t)), &written, nullptr);
    }
    CloseHandle(f);
}

static bool EnableDebugPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
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
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        Log(L"inject: OpenProcess(%lu) failed err=%lu", pid, GetLastError());
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        Log(L"inject: VirtualAllocEx failed err=%lu", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        Log(L"inject: WriteProcessMemory failed err=%lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel, "LoadLibraryW"));
    if (!loadLibrary) {
        Log(L"inject: LoadLibraryW not found");
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        Log(L"inject: CreateRemoteThread failed err=%lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    const bool loaded = exitCode != 0;
    Log(L"inject: pid=%lu base=0x%llX loaded=%d", pid,
        (unsigned long long)exitCode, loaded ? 1 : 0);
    return loaded;
}

static std::wstring AppNameFromTitle(std::wstring title) {
    static const wchar_t* kPrefixes[] = {
        L"Jump List for ",
        L"Liste de raccourcis ",
        L"Liste des raccourcis ",
    };
    for (auto* p : kPrefixes) {
        const size_t n = wcslen(p);
        if (title.size() >= n && _wcsnicmp(title.c_str(), p, n) == 0)
            return title.substr(n);
    }
    return title;
}

static void HandleRequest() {
    HANDLE f = CreateFileW(kReq, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    wchar_t buf[512] = {};
    DWORD read = 0;
    ReadFile(f, buf, (DWORD)sizeof(buf) - sizeof(wchar_t), &read, nullptr);
    CloseHandle(f);
    if (read < sizeof(wchar_t)) return;

    std::wstring title(buf, read / sizeof(wchar_t));
    while (!title.empty()) {
        const wchar_t c = title.back();
        if (c == L'\r' || c == L'\n' || c == L'\0') title.pop_back();
        else break;
    }
    if (title.empty()) return;

    const std::wstring app = AppNameFromTitle(title);
    Log(L"request '%s' -> '%s'", title.c_str(), app.c_str());

    TaskbarHit hit;
    hit.onButton = true;
    hit.title = app;

    const auto pids = ResolveProcesses(hit);
    if (pids.empty()) {
        Log(L"no process match for '%s'", app.c_str());
        return;
    }

    for (DWORD pid : pids)
        Log(L"target pid=%lu %s", pid, ProcessImagePath(pid).c_str());

    const int killed = KillProcesses(pids);
    Log(L"killed %d/%zu for '%s'", killed, pids.size(), app.c_str());
}

static void EnsureInjected() {
    const auto pids = FindProcesses(L"ShellExperienceHost.exe");
    for (DWORD pid : pids) {
        if (!ModuleLoaded(pid, L"EndTaskHook.dll")) {
            Log(L"SEH pid %lu missing hook, injecting", pid);
            Inject(pid, kDll);
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HANDLE once = CreateMutexW(nullptr, TRUE, L"EndTaskHelper_SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    if (!EnableDebugPrivilege())
        Log(L"warning: SeDebugPrivilege not enabled err=%lu", GetLastError());

    Log(L"helper start pid=%lu", GetCurrentProcessId());

    FILETIME lastReq{};
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(kReq, GetFileExInfoStandard, &fad))
        lastReq = fad.ftLastWriteTime;

    ULONGLONG lastInject = 0;
    for (;;) {
        const ULONGLONG now = GetTickCount64();

        if (now - lastInject >= 1000) {
            lastInject = now;
            EnsureInjected();
        }

        if (GetFileAttributesExW(kReq, GetFileExInfoStandard, &fad)) {
            if (CompareFileTime(&fad.ftLastWriteTime, &lastReq) != 0) {
                lastReq = fad.ftLastWriteTime;
                HandleRequest();
            }
        } else {
            lastReq = FILETIME{};
        }

        Sleep(200);
    }
}
