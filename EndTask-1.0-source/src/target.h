#pragma once
#include <windows.h>
#include <string>
#include <vector>

// What the user right-clicked on, on the taskbar.
struct TaskbarHit {
    bool onButton = false;
    std::wstring appId;   // parsed from AutomationId "Appid:..."
    std::wstring title;   // UIA accessible name of the button
};

// Returns onButton=true only when the point is over a taskbar app button.
TaskbarHit HitTestTaskbarButton(POINT pt);

// Maps the hit to the process ids that own the matching windows.
std::vector<DWORD> ResolveProcesses(const TaskbarHit& hit);

// Terminates every pid in the list. Returns how many were killed.
int KillProcesses(const std::vector<DWORD>& pids);
