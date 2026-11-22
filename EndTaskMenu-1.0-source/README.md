# EndTaskMenu - Source

Full buildable source for the **"End task"** jump-list row.

The row appears directly above **"Close window"** for running apps and kills
the app's process when clicked. The native XAML menu of
`ShellExperienceHost.exe` is modified in memory by an injected DLL. Nothing is
drawn as an overlay.

## Project layout

```
hook/
  hookdll.cpp        injected DLL: XAML lookup, row insertion, click handling
  inject.cpp         standalone test injector (EndTaskInject.exe)

helper/
  helper.cpp         elevated watchdog: injection loop + click request + kill

manager/
  manager.cpp        UAC manager: install / uninstall / status + console menu

src/
  target.cpp/.h      resolve the target process from a jump-list title, kill it
  util.cpp/.h        UTF-16, path, and logging helpers

README.md            this file
```

## How it works

1. The manager copies `EndTaskHook.dll` and `EndTaskHelper.exe` to
   `C:\ProgramData\EndTaskMenu`, creates the `EndTaskMenu` scheduled task at
   logon with highest privileges, and starts the helper.
2. `EndTaskHelper.exe` checks every second whether the DLL is loaded in
   `ShellExperienceHost.exe`. If not, it injects it with
   `CreateRemoteThread(LoadLibraryW)`.
3. The DLL finds the jump-list `CoreWindow`, subclasses its window procedure,
   and walks the XAML tree to `SystemItemList`.
4. For a **running** app, the native `Close` row exists. The DLL inserts a
   32 px `ListViewItem` labeled **"End task"** directly above it, copying the
   native font, foreground color, and icon style. It compresses the star-sized
   list instead of growing the window: the system block gets
   `rows * 32 + 14` px plus the remaining list height.
5. For a **closed** app, there is no `Close` row. The DLL removes any stale
   row, restores `SystemItemList.Height` to `nativeRows * 32`, restores the
   border to `height + 14`, and calls `FitWindowToContent`. Closed apps never
   show the row.
6. On click, the DLL writes the jump-list title to
   `C:\ProgramData\EndTaskMenu\request.txt`. The helper reads the title,
   resolves the process through visible windows and executable names (with a
   special case for Explorer), and terminates it.
7. The shell closes the menu when the process dies. If the process cannot be
   killed, the DLL sends a real Escape key to the flyout after 400 ms.

A small edge case is handled in the DLL: when a tiny closed menu (under 100 px
client height) is reused for a running app, the grow is deferred by 350 ms so
the shell has time to paint the system block.

## Build

Open an **x64 Native Tools Command Prompt for VS 2022** and run these commands
from the repository root. There is no build script.

```bat
mkdir build
mkdir dist

cl /nologo /LD /std:c++17 /O2 /Oi /Gy /Gw /GL /DNDEBUG /EHsc /W4 /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Fo:build\ /Fe:build\EndTaskHook.dll ^
   hook\hookdll.cpp ^
   /link /SUBSYSTEM:WINDOWS /DLL /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
   WindowsApp.lib comctl32.lib ole32.lib user32.lib advapi32.lib

cl /nologo /std:c++17 /O2 /Oi /Gy /DNDEBUG /EHsc /W4 /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Fo:build\ /Fe:build\EndTaskInject.exe ^
   hook\inject.cpp ^
   /link /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF advapi32.lib

cl /nologo /std:c++17 /O2 /Oi /Gy /Gw /GL /DNDEBUG /EHsc /W4 /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /DNOMINMAX /Isrc ^
   /Fo:build\ /Fe:build\EndTaskHelper.exe ^
   helper\helper.cpp src\target.cpp src\util.cpp ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
   /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   uiautomationcore.lib oleacc.lib ole32.lib oleaut32.lib uuid.lib shell32.lib shlwapi.lib advapi32.lib user32.lib dwmapi.lib propsys.lib

cl /nologo /std:c++17 /O2 /Oi /Gy /Gw /GL /DNDEBUG /EHsc /W4 /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Fo:build\ /Fe:build\EndTaskManager.exe ^
   manager\manager.cpp ^
   /link /SUBSYSTEM:CONSOLE /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
   /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   advapi32.lib shell32.lib

copy /y build\EndTaskHook.dll dist\
copy /y build\EndTaskHelper.exe dist\
copy /y build\EndTaskManager.exe dist\
copy /y README.md dist\
```

Output goes to `dist\`. Requires Visual Studio 2022 (or Build Tools) with the
C++ workload.

## Where the built files go at runtime

```
dist\EndTaskManager.exe    -> run this to install
dist\EndTaskHelper.exe     -> copied to C:\ProgramData\EndTaskMenu\
dist\EndTaskHook.dll       -> copied to C:\ProgramData\EndTaskMenu\
Task Scheduler             -> EndTaskMenu, at logon, highest privileges
HKCU\Software\EndTaskMenu  -> Enabled = 0/1
```

The click request and helper log also live in `C:\ProgramData\EndTaskMenu\`.

## Limitations

- UWP apps hosted by `ApplicationFrameHost.exe` are not killed, because the
  host is shared with other apps.
- A closed or pinned app has no process to kill, so no row is added.
- Right after installation, if `ShellExperienceHost.exe` was not running, the
  first jump list can open before injection. The next one is fine.
