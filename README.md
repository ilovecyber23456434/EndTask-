# EndTaskMenu 1.0

Adds a real **"End task"** row directly above **"Close window"** in the
taskbar jump list, then kills that app's process when the row is clicked.

Everything is installed by `EndTaskManager.exe`. There is **no install.bat**
and nothing else to run by hand.

---

## Quick start

1. Extract `EndTaskMenu-1.0-bin.zip` anywhere.
2. Double-click **`EndTaskManager.exe`**.
3. Approve the UAC prompt.
4. Choose **`1) Install / enable`**.
5. Right-click the taskbar button of an app that is **running**.
   The row **"End task"** appears directly above **"Close window"**.
6. Click it: the app is killed and the menu closes.

To uninstall or check the status, run `EndTaskManager.exe` again:

- `2) Uninstall / disable`
- `3) Status`

---

## What is in the binary package

```
EndTaskManager.exe   <-- RUN THIS. Installer / uninstaller / status.
EndTaskHelper.exe        Background helper. Started automatically. Do not run it.
EndTaskHook.dll          DLL injected into ShellExperienceHost.exe.
README.md
```

### Why are there two .exe files?

- **`EndTaskManager.exe`** is the manager. It requests administrator rights,
  copies the files to `C:\ProgramData\EndTaskMenu`, creates the `EndTaskMenu`
  scheduled task, starts the helper, and can uninstall everything.
- **`EndTaskHelper.exe`** is the background worker. It runs elevated, watches
  `ShellExperienceHost.exe`, injects `EndTaskHook.dll`, and performs the kill
  when the row is clicked. The manager starts it automatically at install and
  Windows starts it at every logon. You never launch it manually.

If you only remember one thing: **run `EndTaskManager.exe`**.

---

## What gets installed

- `C:\ProgramData\EndTaskMenu\EndTaskHook.dll`
- `C:\ProgramData\EndTaskMenu\EndTaskHelper.exe`
- Scheduled task `EndTaskMenu` (at logon, highest privileges)

Uninstall removes all of it and restarts `ShellExperienceHost.exe` to unload
the DLL.

---

## Source package

`EndTaskMenu-1.0-source.zip` contains the full buildable source:

```
hook/                  injected DLL + test injector
helper/                watchdog + kill
manager/               UAC installer
src/                   target resolution and utilities
README.md
```

Build:

The exact `cl` commands are in the `Build` section of `README.md` inside the
source package. Requires Visual Studio 2022 (or Build Tools) with the C++
workload. Output goes to `dist\`.

---

## Behavior and limitations

- The row only appears for apps that are **running**. A closed or pinned app
  has no native "Close window" row, so no "End task" row is added.
- UWP apps hosted by `ApplicationFrameHost.exe` are not killed, because that
  host is shared with other apps.
- Classic Win32 apps, Explorer, Discord, browsers, and similar apps work.
- Right after installation, if `ShellExperienceHost.exe` was not running, the
  first jump list may open before the DLL is injected. The next one is fine;
  the helper also re-injects after every shell restart.

## Package checksums (SHA256)

```
EndTaskMenu-1.0-bin.zip
02888220BE1F03DDCC09D195F23250AA6B13F88EBE5A803870C1C77370FD40A3

EndTaskMenu-1.0-source.zip
563D58EF3806CED2EEBDC35373B10CBABE66CB3C01652BE1A3B52C6FDB75E605
```
