#include "target.h"
#include "util.h"
#include <ole2.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <algorithm>

// ---------------------------------------------------------------- UIA glue

static IUIAutomation* Uia() {
    static IUIAutomation* uia = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&uia));
    }
    return uia;
}

static std::wstring CurrentString(IUIAutomationElement* e,
                                  HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*getter)(BSTR*)) {
    BSTR b = nullptr;
    std::wstring s;
    if (SUCCEEDED((e->*getter)(&b)) && b) {
        s.assign(b, SysStringLen(b));
        SysFreeString(b);
    }
    return s;
}

// ------------------------------------------------------------- hit testing

static bool PointOnTaskbar(POINT pt) {
    struct Ctx { POINT pt; bool hit; } ctx{pt, false};

    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64];
        if (!GetClassNameW(h, cls, 64)) return TRUE;
        if (_wcsicmp(cls, L"Shell_TrayWnd") && _wcsicmp(cls, L"Shell_SecondaryTrayWnd"))
            return TRUE;
        RECT r;
        if (GetWindowRect(h, &r) && PtInRect(&r, c->pt)) {
            c->hit = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.hit;
}

static bool TitleMatchesVisibleWindow(const std::wstring& title) {
    struct Ctx { const std::wstring* t; bool hit; } ctx{&title, false};
    if (title.empty()) return false;

    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h) || IsCloaked(h)) return TRUE;
        if (GetAncestor(h, GA_ROOT) != h) return TRUE;
        if (GetWindow(h, GW_OWNER)) return TRUE;
        wchar_t buf[512] = {};
        GetWindowTextW(h, buf, 512);
        if (buf[0] && IEquals(buf, *c->t)) {
            c->hit = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.hit;
}

static TaskbarHit HitTestXamlTaskbarButton(POINT pt) {
    TaskbarHit out;

    IUIAutomation* uia = Uia();
    if (!uia) return out;

    ComPtr<IUIAutomationElement> el;
    if (FAILED(uia->ElementFromPoint(pt, el.put())) || !el) return out;

    ComPtr<IUIAutomationTreeWalker> walker;
    uia->get_ControlViewWalker(walker.put());

    ComPtr<IUIAutomationElement> cur;
    cur.attach(el.get());
    el.get()->AddRef();

    for (int i = 0; i < 5 && cur; ++i) {
        CONTROLTYPEID ct = 0;
        cur->get_CurrentControlType(&ct);
        if (ct == UIA_ButtonControlTypeId) break;
        ComPtr<IUIAutomationElement> parent;
        if (!walker || FAILED(walker->GetParentElement(cur.get(), parent.put())) || !parent) {
            cur.reset();
            break;
        }
        cur = std::move(parent);
    }
    if (!cur) return out;

    CONTROLTYPEID ct = 0;
    cur->get_CurrentControlType(&ct);
    if (ct != UIA_ButtonControlTypeId) return out;

    const std::wstring cls  = CurrentString(cur.get(), &IUIAutomationElement::get_CurrentClassName);
    const std::wstring name = CurrentString(cur.get(), &IUIAutomationElement::get_CurrentName);
    const std::wstring aid  = CurrentString(cur.get(), &IUIAutomationElement::get_CurrentAutomationId);

    if (StartsWithI(aid, L"Appid:")) out.appId = aid.substr(6);
    out.title = name;

    // Gate: only accept things that look like task-list buttons. The tray,
    // clock and start button are buttons too, but never match these rules.
    bool looksLikeTaskButton = IContains(cls, L"tasklistbutton") || !out.appId.empty();
    if (!looksLikeTaskButton && !name.empty())
        looksLikeTaskButton = TitleMatchesVisibleWindow(name);
    if (!looksLikeTaskButton) return out;

    out.onButton = true;
    return out;
}

// ------------------------------------------------- Win10 classic taskbar

static std::wstring StripRunningSuffix(std::wstring s);

static HWND FindClassicTaskList(POINT pt) {
    struct TopCtx { POINT pt; HWND list; };

    TopCtx top{pt, nullptr};
    EnumWindows([](HWND bar, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<TopCtx*>(lp);
        wchar_t cls[64] = {};
        if (!GetClassNameW(bar, cls, 64)) return TRUE;
        if (_wcsicmp(cls, L"Shell_TrayWnd") && _wcsicmp(cls, L"Shell_SecondaryTrayWnd"))
            return TRUE;

        RECT br{};
        if (!GetWindowRect(bar, &br) || !PtInRect(&br, c->pt)) return TRUE;

        struct ChildCtx { POINT pt; HWND* out; };
        ChildCtx cc{c->pt, &c->list};
        EnumChildWindows(bar, [](HWND child, LPARAM lp2) -> BOOL {
            auto* c2 = reinterpret_cast<ChildCtx*>(lp2);
            wchar_t cls2[64] = {};
            if (!GetClassNameW(child, cls2, 64)) return TRUE;
            if (_wcsicmp(cls2, L"MSTaskListWClass")) return TRUE;
            RECT r{};
            if (!GetWindowRect(child, &r) || !PtInRect(&r, c2->pt)) return TRUE;
            *c2->out = child;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&cc));

        return c->list ? FALSE : TRUE;
    }, reinterpret_cast<LPARAM>(&top));

    return top.list;
}

static bool HitTestClassicTaskbar(POINT pt, TaskbarHit& out) {
    HWND list = FindClassicTaskList(pt);
    if (!list) return false;

    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(list, (DWORD)OBJID_CLIENT, IID_IAccessible,
                                          reinterpret_cast<void**>(&acc))) || !acc)
        return false;

    long count = 0;
    if (SUCCEEDED(acc->get_accChildCount(&count))) {
        for (long i = 1; i <= count; ++i) {
            VARIANT child;
            VariantInit(&child);
            child.vt = VT_I4;
            child.lVal = i;

            long x = 0, y = 0, w = 0, h = 0;
            if (SUCCEEDED(acc->accLocation(&x, &y, &w, &h, child)) && w > 0 && h > 0) {
                RECT r{x, y, x + w, y + h};
                if (PtInRect(&r, pt)) {
                    BSTR name = nullptr;
                    if (SUCCEEDED(acc->get_accName(child, &name)) && name) {
                        if (SysStringLen(name) > 0) {
                            out.title = StripRunningSuffix(std::wstring(name, SysStringLen(name)));
                            SysFreeString(name);
                            VariantClear(&child);
                            break;
                        }
                        SysFreeString(name);
                    }
                }
            }
            VariantClear(&child);
        }
    }
    acc->Release();

    out.onButton = !out.title.empty();
    return out.onButton;
}

TaskbarHit HitTestTaskbarButton(POINT pt) {
    TaskbarHit out;
    if (!PointOnTaskbar(pt)) return out;

    out = HitTestXamlTaskbarButton(pt);
    if (out.onButton) return out;

    out = {};
    HitTestClassicTaskbar(pt, out);
    return out;
}

// --------------------------------------------------------------- resolution

static bool SameAppId(const std::wstring& aumid, const std::wstring& appId) {
    if (aumid.empty() || appId.empty()) return false;
    if (IEquals(aumid, appId)) return true;

    // "PackageFamily_8wekyb3d8bbwe!App" style: compare the package part.
    auto family = [](const std::wstring& s) {
        auto p = s.find(L'!');
        return p == std::wstring::npos ? s : s.substr(0, p);
    };
    if (aumid.find(L'!') != std::wstring::npos || appId.find(L'!') != std::wstring::npos)
        return IEquals(family(aumid), family(appId));

    return false;
}

// Win10 taskbar buttons are named "Discord - 1 running window".
static std::wstring StripRunningSuffix(std::wstring s) {
    const auto pos = s.rfind(L" - ");
    if (pos == std::wstring::npos) return s;

    const std::wstring tail = ToLower(s.substr(pos + 3));
    static const wchar_t* kHints[] = {
        L"running window", L"fenêtre", L"fenster", L"ventana", L"finestra",
        L"janela", L"venster", L"okno", L"окн", L"pencere",
        L"窗口", L"ウィンドウ", L"창",
    };
    for (auto* hint : kHints)
        if (tail.find(hint) != std::wstring::npos) return s.substr(0, pos);
    return s;
}

struct Match {
    DWORD pid = 0;
    int score = 0;
};

static int ScoreWindow(const TaskbarHit& hit, HWND h, DWORD pid) {
    wchar_t title[512] = {};
    GetWindowTextW(h, title, 512);
    const std::wstring t = title;

    // The jump list itself is a top-level window titled "Jump List for X".
    // It contains the app name and would turn a single weak match into an
    // ambiguous one, so never score shell flyouts.
    if (StartsWithI(t, L"Jump List") ||
        StartsWithI(t, L"Liste de raccourcis") ||
        StartsWithI(t, L"Liste des raccourcis"))
        return 0;

    const std::wstring exe = ProcessImagePath(pid);
    const std::wstring exeName = FileNameOf(exe);
    std::wstring base = exeName;
    const auto dot = base.rfind(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);

    int score = 0;
    auto bump = [&](int v) { if (v > score) score = v; };

    if (!hit.appId.empty()) {
        if (SameAppId(GetWindowAumid(h), hit.appId)) bump(100);
        if (IEquals(exe, hit.appId) || IEquals(exeName, hit.appId) || IEquals(base, hit.appId))
            bump(100);
    }

    if (!hit.title.empty()) {
        if (IEquals(t, hit.title)) bump(95);
        if (IEquals(base, hit.title) || IEquals(exeName, hit.title)) bump(92);
        if (StartsWithI(t, hit.title + L" - ") || StartsWithI(t, hit.title + L" | "))
            bump(85);

        // Explorer's process name never matches its button label.
        if ((IEquals(hit.title, L"File Explorer") || IEquals(hit.title, L"Explorateur de fichiers")) &&
            IEquals(base, L"explorer"))
            bump(100);

        // Weak signals, only used when nothing better exists.
        if (IContains(t, hit.title)) bump(55);
        if (IContains(base, hit.title)) bump(50);
    }

    return score;
}

struct EnumCtx {
    const TaskbarHit* hit;
    std::vector<Match> matches;
};

static BOOL CALLBACK WindowProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);

    if (!IsWindowVisible(h) || IsCloaked(h)) return TRUE;
    if (GetAncestor(h, GA_ROOT) != h) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;

    wchar_t cls[128] = {};
    if (GetClassNameW(h, cls, 128)) {
        if (!_wcsicmp(cls, L"Progman") || !_wcsicmp(cls, L"WorkerW") ||
            !_wcsicmp(cls, L"Shell_TrayWnd") || !_wcsicmp(cls, L"Shell_SecondaryTrayWnd") ||
            !_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") ||
            !_wcsicmp(cls, L"ApplicationFrameWindow"))
            return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == GetCurrentProcessId()) return TRUE;

    const int score = ScoreWindow(*c->hit, h, pid);
    if (score <= 0) return TRUE;

    for (auto& m : c->matches) {
        if (m.pid == pid) {
            if (score > m.score) m.score = score;
            return TRUE;
        }
    }
    c->matches.push_back(Match{pid, score});
    return TRUE;
}

std::vector<DWORD> ResolveProcesses(const TaskbarHit& hit) {
    EnumCtx ctx{&hit, {}};
    EnumWindows(WindowProc, reinterpret_cast<LPARAM>(&ctx));

    int top = 0;
    for (const auto& m : ctx.matches) top = std::max(top, m.score);

    std::vector<DWORD> out;
    if (top >= 80) {
        for (const auto& m : ctx.matches)
            if (m.score >= 80) out.push_back(m.pid);
    } else if (top >= 55) {
        std::vector<DWORD> weak;
        for (const auto& m : ctx.matches)
            if (m.score >= 55) weak.push_back(m.pid);
        if (weak.size() == 1) out = weak;
    }

    // Old UWP hosts are shared by several apps: never kill them.
    std::vector<DWORD> filtered;
    for (DWORD pid : out) {
        const std::wstring exe = ProcessImagePath(pid);
        if (IEquals(FileNameOf(exe), L"ApplicationFrameHost.exe")) continue;
        if (std::find(filtered.begin(), filtered.end(), pid) == filtered.end())
            filtered.push_back(pid);
    }
    return filtered;
}

int KillProcesses(const std::vector<DWORD>& pids) {
    int killed = 0;
    for (DWORD pid : pids) {
        if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!h) continue;
        if (TerminateProcess(h, 1)) {
            ++killed;
            WaitForSingleObject(h, 400);
        }
        CloseHandle(h);
    }
    return killed;
}
