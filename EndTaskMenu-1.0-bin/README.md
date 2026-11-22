# EndTaskMenu - Binary

Adds a real **"End task"** row directly above **"Close window"** in the
taskbar jump list, then kills that app's process when the row is clicked.

This is not an overlay. The native XAML menu of `ShellExperienceHost.exe` is
modified in memory by an injected DLL.

## Requirements

- Windows 10 or Windows 11, x64
- Administrator rights for installation
- Standard Windows shell (`explorer.exe` + `ShellExperienceHost.exe`)

## Quick start

1. Double-click **`EndTaskManager.exe`**.
2. Approve the UAC prompt.
3. Choose **`1) Install / enable`**.
4. Right-click the taskbar button of an app that is **running**.
5. The row **"End task"** appears directly above **"Close window"**.
6. Click it. The app is killed and the menu closes.

After Install, Uninstall, or Status, press Enter. The console clears and the
main menu comes back.

## Files in this folder

```
EndTaskManager.exe   manager. Run this for install / uninstall / status.
EndTaskHelper.exe    background helper. Started automatically. Do not run it.
EndTaskHook.dll      DLL injected into ShellExperienceHost.exe.
README.md            this file.
```

## Why two .exe files

- **`EndTaskManager.exe`** requests administrator rights, copies the runtime
  files to `C:\ProgramData\EndTaskMenu`, creates the `EndTaskMenu` scheduled
  task, starts the helper, and verifies that the hook is loaded. It also
  removes everything on uninstall.
- **`EndTaskHelper.exe`** runs elevated in the background. It watches
  `ShellExperienceHost.exe`, injects `EndTaskHook.dll` whenever the shell
  restarts, and kills the target process when the row is clicked. The manager
  starts it at install; Windows starts it at every logon.

## How it works

1. The helper checks every second whether `EndTaskHook.dll` is loaded in
   `ShellExperienceHost.exe`. If it is missing, it injects it with
   `CreateRemoteThread(LoadLibraryW)`.
2. The DLL finds the jump-list `CoreWindow`, subclasses its window procedure,
   and walks the XAML tree to `SystemItemList`.
3. When the menu opens for a **running** app, the native `Close` row exists.
   The DLL inserts a 32 px `ListViewItem` labeled **"End task"** directly above
   it, copying the native font, foreground color, and icon style.
4. When the menu opens for a **closed** app, there is no `Close` row. The DLL
   removes any stale `End task` row, restores the list height and border, and
   fits the window back to the native content height. No blank row is left.
5. On click, the DLL writes the jump-list title to
   `C:\ProgramData\EndTaskMenu\request.txt`. The helper reads it, resolves the
   app's process, and terminates it.
6. The shell closes the menu when the process dies. If the process cannot be
   killed (shared UWP host), the DLL sends a real Escape key to the flyout
   after 400 ms so the menu still closes.

## What the manager installs

```
C:\ProgramData\EndTaskMenu\EndTaskHook.dll      injected DLL
C:\ProgramData\EndTaskMenu\EndTaskHelper.exe   elevated helper
C:\ProgramData\EndTaskMenu\request.txt         click request (temporary)
C:\ProgramData\EndTaskMenu\helper.log          helper log
Task Scheduler -> EndTaskMenu                  logon task, highest privileges
HKCU\Software\EndTaskMenu                      Enabled = 0/1
```

It also grants the AppContainer SID access to the install folder so the
injected DLL can write `request.txt`.

## Uninstall and status

Run `EndTaskManager.exe` again:

- `2) Uninstall / disable` stops the task and helper, restarts
  `ShellExperienceHost.exe` to unload the DLL, and deletes
  `C:\ProgramData\EndTaskMenu`.
- `3) Status` shows the scheduled task, helper process, hook injection, and
  installed DLL.

## Limitations

- UWP apps hosted by `ApplicationFrameHost.exe` are not killed, because the
  host is shared with other apps.
- A closed or pinned app has no process to kill, so no row is added.
- Right after installation, if `ShellExperienceHost.exe` was not running, the
  first jump list can open before injection. The next one is fine.
