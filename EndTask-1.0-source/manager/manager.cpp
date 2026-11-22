// EndTaskManager.exe - elevated installer/manager for the "Terminer la tache"
// jump-list row.
//
//   1) Installer / activer
//   2) Desinstaller / desactiver
//   3) Quitter
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
                        wprintf(L"  [+] watcher de test arrete (pid %lu)\n", pe.th32ProcessID);
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
        wprintf(L"  [!] copie %s -> %s : erreur %lu\n",
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
    wprintf(L"\n=== Installation / activation ===\n");

    wprintf(L"[*] Arret de l'ancienne version / overlay / watcher...\n");
    Run(L"schtasks /end /tn \"" + std::wstring(kTask) + L"\"");
    Run(L"schtasks /delete /tn \"" + std::wstring(kTask) + L"\" /f");
    Run(L"taskkill /f /im EndTaskHelper.exe");
    Run(L"taskkill /f /im EndTaskMenu.exe");
    KillLegacyWatchers();
    DeleteValue(kRunKey, L"EndTaskMenu");

    wprintf(L"[*] Redemarrage de ShellExperienceHost (decharge les anciens hooks)...\n");
    Run(L"taskkill /f /im ShellExperienceHost.exe");
    Sleep(2500);

    wprintf(L"[*] Copie des fichiers...\n");
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

    wprintf(L"[*] Tache planifiee au logon...\n");
    const std::wstring tr = std::wstring(L"\"") + kDir + L"\\" + kHelp + L"\"";
    const std::wstring create =
        L"schtasks /create /tn \"" + std::wstring(kTask) + L"\" /tr " + tr +
        L" /sc onlogon /rl highest /f";
    if (!RunOk(create))
        wprintf(L"  [!] schtasks create a echoue\n");

    wprintf(L"[*] Demarrage du helper...\n");
    RunOk(L"schtasks /run /tn \"" + std::wstring(kTask) + L"\"");

    wprintf(L"[*] Verification du hook...\n");
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
    wprintf(L"  hook injecte : %s\n", hook ? L"oui" : L"non");

    WriteDword(kCfgKey, L"Enabled", 1);

    wprintf(L"\n[ok] Installe. Ouvre une jump list : la row \"Terminer la tache\"\n");
    wprintf(L"     doit apparaitre juste au-dessus de \"Fermer la fenetre\".\n");
    if (!hook)
        wprintf(L"     ShellExperienceHost n'etait pas encore lance : la premiere\n"
                L"     jump list peut arriver avant l'injection, la suivante est bonne.\n");
}

static void Uninstall() {
    wprintf(L"\n=== Desinstallation / desactivation ===\n");

    wprintf(L"[*] Arret de la tache...\n");
    Run(L"schtasks /end /tn \"" + std::wstring(kTask) + L"\"");
    Run(L"schtasks /delete /tn \"" + std::wstring(kTask) + L"\" /f");

    wprintf(L"[*] Arret du helper / watcher...\n");
    Run(L"taskkill /f /im EndTaskHelper.exe");
    KillLegacyWatchers();
    Sleep(500);

    wprintf(L"[*] Redemarrage de ShellExperienceHost pour decharger le hook...\n");
    Run(L"taskkill /f /im ShellExperienceHost.exe");
    Sleep(2500);

    wprintf(L"[*] Suppression des fichiers...\n");
    Run(L"cmd /c rmdir /s /q \"C:\\ProgramData\\EndTaskMenu\"");

    DeleteValue(kRunKey, L"EndTaskMenu");
    WriteDword(kCfgKey, L"Enabled", 0);

    wprintf(L"\n[ok] Desinstalle. La row ne doit plus apparaitre.\n");
}

static void Status() {
    wprintf(L"\n=== Etat ===\n");

    const bool task = RunOk(L"schtasks /query /tn \"" + std::wstring(kTask) + L"\"");
    wprintf(L"  tache planifiee : %s\n", task ? L"oui" : L"non");

    const auto helpers = FindProcesses(L"EndTaskHelper.exe");
    wprintf(L"  helper          : %s", helpers.empty() ? L"non" : L"oui");
    if (!helpers.empty()) wprintf(L" (pid %lu)", helpers.front());
    wprintf(L"\n");

    bool hook = false;
    for (DWORD pid : FindProcesses(L"ShellExperienceHost.exe")) {
        if (ModuleLoaded(pid, L"EndTaskHook.dll")) {
            hook = true;
            break;
        }
    }
    wprintf(L"  hook injecte    : %s\n", hook ? L"oui" : L"non");

    const DWORD attr = GetFileAttributesW(kDll);
    wprintf(L"  DLL installee   : %s\n", attr != INVALID_FILE_ATTRIBUTES ? L"oui" : L"non");
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"Terminer la tache - gestionnaire");

    // Non-interactive modes (install.bat uses the menu; these are for
    // scripting and for the elevated smoke tests).
    if (argc > 1) {
        if (!_wcsicmp(argv[1], L"--install")) { Install(); return 0; }
        if (!_wcsicmp(argv[1], L"--uninstall")) { Uninstall(); return 0; }
        if (!_wcsicmp(argv[1], L"--status")) { Status(); return 0; }
    }

    wprintf(L"==============================================\n");
    wprintf(L"  Terminer la tache - gestionnaire\n");
    wprintf(L"  Row jump list au-dessus de \"Fermer la fenetre\"\n");
    wprintf(L"==============================================\n");

    for (;;) {
        wprintf(L"\n  1) Installer / activer\n");
        wprintf(L"  2) Desinstaller / desactiver\n");
        wprintf(L"  3) Etat\n");
        wprintf(L"  4) Quitter\n");
        wprintf(L"\nChoix : ");

        wchar_t line[16] = {};
        if (!fgetws(line, 16, stdin)) break;

        switch (line[0]) {
        case L'1': Install(); break;
        case L'2': Uninstall(); break;
        case L'3': Status(); break;
        case L'4': return 0;
        default: wprintf(L"  choix invalide\n"); break;
        }
    }
    return 0;
}
