# EndTaskMenu

Adds a real **"End task"** row directly above **"Close window"** in the
taskbar jump list (right-click an app button), then kills that app's process
when the row is clicked.

This is not an overlay: the native XAML menu of `ShellExperienceHost.exe` is
modified in memory by an injected DLL.

## Installation

1. Open the folder that contains `EndTaskManager.exe`.
2. Run `EndTaskManager.exe` (UAC).
3. Choose `1) Install / enable`.
4. Open the jump list of a **running** app: the row appears above
   "Close window". For a closed app, no row appears.

The manager installs:

- `C:\ProgramData\EndTaskMenu\EndTaskHook.dll`
- `C:\ProgramData\EndTaskMenu\EndTaskHelper.exe`
- an `EndTaskMenu` scheduled task (at logon, elevated)

## Uninstall

Run `EndTaskManager.exe`, choose `2) Uninstall / disable`.

The manager:

- removes the scheduled task and the helper;
- restarts `ShellExperienceHost.exe` to unload the DLL;
- deletes `C:\ProgramData\EndTaskMenu`.

## Status

Option `3) Status`: shows whether the task, the helper and the hook are active.

## How it works

1. `EndTaskHelper.exe` runs elevated in the background and checks every second
   that `EndTaskHook.dll` is loaded in `ShellExperienceHost.exe`. If it is not
   (first launch, shell restart), it injects it via
   `CreateRemoteThread(LoadLibraryW)`.
2. The DLL finds the jump list's `CoreWindow`, subclasses its wndproc and
   locates `SystemItemList` in the XAML tree.
3. If the app is running, it inserts a `ListViewItem` "End task" right before
   the native `Close` row, with the same height (32 px) and text style. If the
   `Close` row is missing (closed app), nothing is inserted and the window
   keeps its native height.
4. On click, the DLL writes the jump-list title to
   `C:\ProgramData\EndTaskMenu\request.txt`. The shell closes the menu when the
   app dies; if the process cannot be killed (shared UWP host case), the DLL
   sends a real `Escape` to the flyout after 400 ms to close it.
5. The helper reads the file, extracts the app name, resolves processes through
   visible windows (title / exe name, special case Explorer) and terminates
   them.

## Files

Binary package:

```
EndTaskManager.exe   UAC manager (install / uninstall / status)
EndTaskHelper.exe    elevated helper (injection watchdog + kill)
EndTaskHook.dll      DLL injected into ShellExperienceHost
README.md
```

Source tree:

```
hook/hookdll.cpp       DLL: XAML, insertion, click
hook/inject.cpp        test injector
helper/helper.cpp      watchdog + kill
manager/manager.cpp    UAC installer
src/target.cpp         target process resolution + kill
src/util.cpp           helpers (UTF-16, paths, logging)
```

## Build

Open an **x64 Native Tools Command Prompt for VS 2022** and run these commands
from the repository root. No build script is required.

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

Output in `dist\`.

## Known limitations

- UWP apps hosted by `ApplicationFrameHost.exe` are not killed (the host is
  shared, so it is left alone). Classic Win32 apps, Explorer, Discord,
  browsers, etc. work.
- A closed app has no process to kill: the row does not appear at all (there
  is no "Close window" in its menu).
- Right after installation, if `ShellExperienceHost.exe` was not running, the
  first jump list may open before injection. The next one is fine; the helper
  also re-injects after every shell restart.
