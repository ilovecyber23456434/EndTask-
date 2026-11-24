# EndTaskMenu 1.0

Adds a real **"End task"** row directly above **"Close window"** in the
taskbar jump list, then kills that app's process when the row is clicked.

## Release folders

```
EndTaskMenu-1.0-bin\      runnable package (manager, helper, DLL)
EndTaskMenu-1.0-source\   complete source code, ready to compile
README.md                 this file
```

Everything is provided as folders: no install script, no build script.

## Quick start

1. Open `EndTaskMenu-1.0-bin`.
2. Double-click **`EndTaskManager.exe`**.
3. Approve the UAC prompt.
4. Choose **`1) Install / enable`**.
5. Right-click the taskbar button of an app that is **running**.
6. Click **"End task"**, directly above **"Close window"**.

After Install, Uninstall, or Status, press Enter. The manager clears the
console and returns to the main menu.

To disable it, run `EndTaskManager.exe` again and choose
`2) Uninstall / disable`. Option `3) Status` shows what is active.

## The two .exe files

- **`EndTaskManager.exe`** is the manager. Run this one. It installs,
  uninstalls, shows status, and starts the helper.
- **`EndTaskHelper.exe`** is the background worker. The manager starts it and
  Windows restarts it at logon. Do not launch it manually.

## How it works

1. `EndTaskHelper.exe` runs elevated and checks every second whether
   `EndTaskHook.dll` is loaded in `ShellExperienceHost.exe`. If not, it injects
   the DLL with `CreateRemoteThread(LoadLibraryW)`. The logon task is created
   with the battery power gates off and no time limit, so the helper starts and
   stays alive on battery too.
2. The DLL attaches to the jump-list `CoreWindow` and locates the
   `SystemItemList` element in the native XAML tree.
3. If the app is running, the native `Close` row exists, so the DLL inserts an
   `End task` row directly above it. The row is 32 px high and copies the
   native font and colors.
4. If the app is closed, there is no `Close` row. The DLL removes any stale
   `End task` row and restores the menu to its native height. Closed apps never
   show the row.
5. On click, the DLL writes the jump-list title to
   `C:\ProgramData\EndTaskMenu\request.txt`. The helper reads it, resolves the
   target process, and terminates it. The shell closes the menu when the app
   dies.
6. If the process cannot be killed, the DLL sends a real Escape key to the
   flyout after 400 ms so the menu still closes.

## Where things are

```
C:\ProgramData\EndTaskMenu\EndTaskHook.dll      installed DLL
C:\ProgramData\EndTaskMenu\EndTaskHelper.exe   installed helper
C:\ProgramData\EndTaskMenu\request.txt         click request (temporary)
C:\ProgramData\EndTaskMenu\helper.log          helper log
Task Scheduler -> EndTaskMenu                  logon task, highest privileges
HKCU\Software\EndTaskMenu                      Enabled = 0/1
ShellExperienceHost.exe                        process that hosts the hook
```

## Source code

The complete code is in `EndTaskMenu-1.0-source\`:

```
hook/                  injected DLL + test injector
helper/                watchdog + kill
manager/               installer / uninstaller / status menu
src/                   process resolution and utilities
README.md              build steps and implementation notes
```

The exact `cl` commands to compile everything are in the `Build` section of
`EndTaskMenu-1.0-source\README.md`. Requires Visual Studio 2022 (or Build
Tools) with the C++ workload. Output goes to `dist\`.

## Limitations

- UWP apps hosted by `ApplicationFrameHost.exe` are not killed, because that
  host is shared with other apps.
- A closed or pinned app has no process to kill, so no row is added.
- Right after installation, if `ShellExperienceHost.exe` was not running, the
  first jump list can open before injection. The next one is fine.
