// EndTaskManager.exe - elevated installer/manager for the "End task"
// jump-list row.
//
//   1) Install / enable
//   2) Uninstall / disable
//   3) Quit
//
// The manifest requests administrator, so double-clicking raises UAC once.
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>

static const wchar_t* kDir   = L"C:\\ProgramData\\EndTaskMenu";
static const wchar_t* kTask  = L"EndTaskMenu";
static const wchar_t* kDll   = L"EndTaskHook.dll";
static const wchar_t* kHelp  = L"EndTaskHelper.exe";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kCfgKey = L"Software\\EndTaskMenu";

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (slash) *slash = 0;
    return buf;
}

static DWORD Run(const std::wstring& cmdline) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return GetLastError();
    }

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static bool RunOk(const std::wstring& cmdline) {
    return Run(cmdline) == 0;
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

typedef NTSTATUS (NTAPI *NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

// Older test runs used a PowerShell watcher (endtask_watcher.ps1) that also
// polls request.txt. It is elevated, so only this elevated manager can stop
// it. Match on the command line, never on "powershell.exe" alone.
static void KillLegacyWatchers() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = ntdll
        ? (NtQueryInformationProcessFn)GetProcAddress(ntdll, "NtQueryInformationProcess")
        : nullptr;
    if (!query) return;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    const DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) continue;
            if (_wcsicmp(pe.szExeFile, L"powershell.exe") &&
                _wcsicmp(pe.szExeFile, L"pwsh.exe"))
                continue;

            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                   FALSE, pe.th32ProcessID);
            if (!h) continue;

            ULONG size = 0;
            query(h, 60 /* ProcessCommandLineInformation */, nullptr, 0, &size);
            if (size) {
                std::vector<BYTE> buf(size);
                if (query(h, 60, buf.data(), size, &size) >= 0) {
                    auto* us = reinterpret_cast<UNICODE_STRING*>(buf.data());
                    std::wstring cmd(us->Buffer, us->Length / sizeof(wchar_t));
                    if (cmd.find(L"endtask_watcher") != std::wstring::npos) {
                        wprintf(L"  [+] legacy test watcher stopped (pid %lu)\n", pe.th32ProcessID);
                        TerminateProcess(h, 0);
                    }
                }
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static bool SamePath(const std::wstring& a, const std::wstring& b) {
    wchar_t fa[MAX_PATH * 2] = {}, fb[MAX_PATH * 2] = {};
    if (!GetFullPathNameW(a.c_str(), MAX_PATH * 2, fa, nullptr)) return false;
    if (!GetFullPathNameW(b.c_str(), MAX_PATH * 2, fb, nullptr)) return false;
    return _wcsicmp(fa, fb) == 0;
}

static bool CopyFileToDir(const std::wstring& name) {
    const std::wstring src = ExeDir() + L"\\" + name;
    const std::wstring dst = std::wstring(kDir) + L"\\" + name;
    if (SamePath(src, dst)) return true;
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        wprintf(L"  [!] copy %s -> %s : error %lu\n",
                src.c_str(), dst.c_str(), GetLastError());
        return false;
    }
    wprintf(L"  [+] %s\n", dst.c_str());
    return true;
}

static void WriteDword(const wchar_t* sub, const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

static void DeleteValue(const wchar_t* sub, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, name);
    RegCloseKey(key);
}

static void Install() {
    wprintf(L"\n=== Install / enable ===\n");

    wprintf(L"[*] Stopping old version / overlay / watcher...\n");
    Run(L"schtasks /end /tn \"" + std::wstring(kTask) + L"\"");
    Run(L"schtasks /delete /tn \"" + std::wstring(kTask) + L"\" /f");
    Run(L"taskkill /f /im EndTaskHelper.exe");
    Run(L"taskkill /f /im EndTaskMenu.exe");
    KillLegacyWatchers();
    DeleteValue(kRunKey, L"EndTaskMenu");

    wprintf(L"[*] Restarting ShellExperienceHost (unloads old hooks)...\n");
    Run(L"taskkill /f /im ShellExperienceHost.exe");
    Sleep(2500);

    wprintf(L"[*] Copying files...\n");
    CreateDirectoryW(kDir, nullptr);
    Run(L"cmd /c del /f /q \"C:\\ProgramData\\EndTaskMenu\\EndTaskHook*.dll\" "
        L"\"C:\\ProgramData\\EndTaskMenu\\EndTaskHelper.exe\" "
        L"\"C:\\ProgramData\\EndTaskMenu\\request.txt\" "
        L"\"C:\\ProgramData\\EndTaskMenu\\watcher.log\" "
        L"\"C:\\ProgramData\\EndTaskMenu\\helper.log\"");
    CopyFileToDir(kDll);
    CopyFileToDir(kHelp);

    wprintf(L"[*] ACL AppContainer...\n");
    Run(L"icacls \"C:\\ProgramData\\EndTaskMenu\" /grant \"*S-1-15-2-1:(OI)(CI)M\" /T /C");

    wprintf(L"[*] Logon scheduled task...\n");
    const std::wstring tr = std::wstring(L"\"") + kDir + L"\\" + kHelp + L"\"";
    const std::wstring create =
        L"schtasks /create /tn \"" + std::wstring(kTask) + L"\" /tr " + tr +
        L" /sc onlogon /rl highest /f";
    if (!RunOk(create))
        wprintf(L"  [!] schtasks create failed\n");

    wprintf(L"[*] Starting helper...\n");
    RunOk(L"schtasks /run /tn \"" + std::wstring(kTask) + L"\"");

    wprintf(L"[*] Verifying hook...\n");
    bool hook = false;
    for (int i = 0; i < 10 && !hook; ++i) {
        Sleep(500);
        for (DWORD pid : FindProcesses(L"ShellExperienceHost.exe")) {
            if (ModuleLoaded(pid, L"EndTaskHook.dll")) {
                hook = true;
                break;
            }
        }
    }
    wprintf(L"  hook injected : %s\n", hook ? L"yes" : L"no");

    WriteDword(kCfgKey, L"Enabled", 1);

    wprintf(L"\n[ok] Installed. Open a jump list: the \"End task\" row\n");
    wprintf(L"     should appear directly above \"Close window\".\n");
    if (!hook)
        wprintf(L"     ShellExperienceHost was not running yet: the first\n"
                L"     jump list may open before injection; the next one is fine.\n");
}

static void Uninstall() {
    wprintf(L"\n=== Uninstall / disable ===\n");

    wprintf(L"[*] Stopping task...\n");
    Run(L"schtasks /end /tn \"" + std::wstring(kTask) + L"\"");
    Run(L"schtasks /delete /tn \"" + std::wstring(kTask) + L"\" /f");

    wprintf(L"[*] Stopping helper / watcher...\n");
    Run(L"taskkill /f /im EndTaskHelper.exe");
    KillLegacyWatchers();
    Sleep(500);

    wprintf(L"[*] Restarting ShellExperienceHost to unload the hook...\n");
    Run(L"taskkill /f /im ShellExperienceHost.exe");
    Sleep(2500);

    wprintf(L"[*] Removing files...\n");
    Run(L"cmd /c rmdir /s /q \"C:\\ProgramData\\EndTaskMenu\"");

    DeleteValue(kRunKey, L"EndTaskMenu");
    WriteDword(kCfgKey, L"Enabled", 0);

    wprintf(L"\n[ok] Uninstalled. The row should no longer appear.\n");
}

static void Status() {
    wprintf(L"\n=== Status ===\n");

    const bool task = RunOk(L"schtasks /query /tn \"" + std::wstring(kTask) + L"\"");
    wprintf(L"  scheduled task  : %s\n", task ? L"yes" : L"no");

    const auto helpers = FindProcesses(L"EndTaskHelper.exe");
    wprintf(L"  helper          : %s", helpers.empty() ? L"no" : L"yes");
    if (!helpers.empty()) wprintf(L" (pid %lu)", helpers.front());
    wprintf(L"\n");

    bool hook = false;
    for (DWORD pid : FindProcesses(L"ShellExperienceHost.exe")) {
        if (ModuleLoaded(pid, L"EndTaskHook.dll")) {
            hook = true;
            break;
        }
    }
    wprintf(L"  hook injected   : %s\n", hook ? L"yes" : L"no");

    const std::wstring dllPath = std::wstring(kDir) + L"\\" + kDll;
    const DWORD attr = GetFileAttributesW(dllPath.c_str());
    wprintf(L"  DLL installed   : %s\n", attr != INVALID_FILE_ATTRIBUTES ? L"yes" : L"no");
}

static void ClearScreen() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (h == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(h, &csbi)) return;

    const DWORD cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    const COORD home{0, 0};
    DWORD written = 0;
    FillConsoleOutputCharacterW(h, L' ', cells, home, &written);
    FillConsoleOutputAttribute(h, csbi.wAttributes, cells, home, &written);
    SetConsoleCursorPosition(h, home);
}

static void Pause() {
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
    wprintf(L"\nPress Enter to return to the menu...");
    wchar_t line[16] = {};
    fgetws(line, 16, stdin);
    wprintf(L"\n");
}

static void ShowMenu() {
    wprintf(L"==============================================\n");
    wprintf(L"  EndTaskMenu - manager\n");
    wprintf(L"  Jump-list row above \"Close window\"\n");
    wprintf(L"==============================================\n");
    wprintf(L"\n  1) Install / enable\n");
    wprintf(L"  2) Uninstall / disable\n");
    wprintf(L"  3) Status\n");
    wprintf(L"  4) Quit\n");
    wprintf(L"\nChoice: ");
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"EndTaskMenu - manager");

    // Non-interactive modes are for scripting and the elevated smoke tests.
    if (argc > 1) {
        if (!_wcsicmp(argv[1], L"--install")) { Install(); return 0; }
        if (!_wcsicmp(argv[1], L"--uninstall")) { Uninstall(); return 0; }
        if (!_wcsicmp(argv[1], L"--status")) { Status(); return 0; }
    }

    ClearScreen();
    ShowMenu();

    for (;;) {
        wchar_t line[16] = {};
        if (!fgetws(line, 16, stdin)) break;

        switch (line[0]) {
        case L'1': Install(); Pause(); break;
        case L'2': Uninstall(); Pause(); break;
        case L'3': Status(); Pause(); break;
        case L'4': return 0;
        default: wprintf(L"  invalid choice\n"); Pause(); break;
        }

        ClearScreen();
        ShowMenu();
    }
    return 0;
}
