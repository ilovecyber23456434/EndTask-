// EndTaskHook.dll - injected into ShellExperienceHost.
// Watches for the XAML jump-list CoreWindow, subclasses it, and dumps the
// XAML visual tree on the UI thread so we can see how the system section is
// built. Probe stage only.
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <roapi.h>
#undef GetCurrentTime
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Data.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.ApplicationModel.Core.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#define WM_HOOK_DUMP (WM_APP + 0x411)
#define WM_HOOK_DISMISS (WM_APP + 0x412)
#define SUBCLASS_ID  0xE7D

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::vector<HWND> g_windows;
static ULONGLONG g_lastDump = 0;
static ULONGLONG g_shownAt = 0;
static constexpr UINT_PTR kFitTimer = 1;
static constexpr UINT_PTR kDismissTimer = 2;
static constexpr UINT_PTR kGrowTimer = 3;
static int g_dumps = 0;

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);

    wchar_t path[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"EndTaskHook.log");

    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!f || f == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[4400];
    int n = swprintf_s(line, L"[%02d:%02d:%02d.%03d][%lu] %s\r\n",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       GetCurrentThreadId(), buf);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(f, line, (DWORD)(n * sizeof(wchar_t)), &written, nullptr);
    }
    CloseHandle(f);
}

static std::wstring HStr(const winrt::hstring& s) {
    return std::wstring(s.c_str(), s.size());
}

static std::wstring TypeName(winrt::Windows::Foundation::IInspectable const& o) {
    if (!o) return L"<null>";
    try {
        return HStr(winrt::get_class_name(o));
    } catch (...) {
        return L"<unknown>";
    }
}

using namespace winrt::Windows::UI::Xaml;

static Controls::ListView g_sysList{nullptr};

static void LogIids(winrt::Windows::Foundation::IInspectable const& obj, const wchar_t* what) {
    auto abi = reinterpret_cast<::IInspectable*>(winrt::get_abi(obj));
    if (!abi) {
        Log(L"probe: %s has no abi", what);
        return;
    }
    ULONG count = 0;
    IID* iids = nullptr;
    const HRESULT hr = abi->GetIids(&count, &iids);
    Log(L"probe: %s GetIids hr=0x%08X count=%lu", what, (unsigned)hr, count);
    if (SUCCEEDED(hr) && iids) {
        for (ULONG i = 0; i < count; ++i) {
            wchar_t guid[64] = {};
            StringFromGUID2(iids[i], guid, 64);
            Log(L"probe:   iid[%lu]=%s", i, guid);
        }
        CoTaskMemFree(iids);
    }
}

static void ProbeProps(winrt::Windows::Foundation::IInspectable const& obj) {
    auto cpp = obj.try_as<winrt::Windows::UI::Xaml::Data::ICustomPropertyProvider>();
    Log(L"probe: ICustomPropertyProvider=%d", cpp ? 1 : 0);
    if (!cpp) return;

    try {
        auto tn = cpp.Type();
        Log(L"probe: provider type kind=%d name='%s'", (int)tn.Kind, HStr(tn.Name).c_str());
    } catch (...) {}

    for (auto* prop : {L"DisplayName", L"AutomationId", L"Command", L"Icon",
                       L"IsEnabled", L"Id", L"Glyph", L"GlyphFontFamily"}) {
        try {
            auto p = cpp.GetCustomProperty(prop);
            Log(L"probe: prop %s -> %d", prop, p ? 1 : 0);
        } catch (winrt::hresult_error const& e) {
            Log(L"probe: prop %s -> hr=0x%08X", prop, (unsigned)e.code());
        } catch (...) {
            Log(L"probe: prop %s -> err", prop);
        }
    }
}

static void ProbeSystemList() {
    if (!g_sysList) {
        Log(L"probe: no system list captured");
        return;
    }

    try {
        auto src = g_sysList.ItemsSource();
        Log(L"probe: ItemsSource class=%s", TypeName(src).c_str());
        LogIids(src, L"ItemsSource");

        auto vec = src.try_as<winrt::Windows::Foundation::Collections::
                                   IVector<winrt::Windows::Foundation::IInspectable>>();
        Log(L"probe: IVector<IInspectable>=%d", vec ? 1 : 0);

        auto obs = src.try_as<winrt::Windows::Foundation::Collections::
                                  IObservableVector<winrt::Windows::Foundation::IInspectable>>();
        Log(L"probe: IObservableVector<IInspectable>=%d", obs ? 1 : 0);
    } catch (winrt::hresult_error const& e) {
        Log(L"probe: source err=0x%08X", (unsigned)e.code());
    } catch (...) {}

    try {
        auto items = g_sysList.Items();
        Log(L"probe: Items.Size=%u", (unsigned)items.Size());
        for (uint32_t i = 0; i < items.Size(); ++i) {
            auto item = items.GetAt(i);
            Log(L"probe: item[%u] class=%s", i, TypeName(item).c_str());
            if (i == 0) {
                LogIids(item, L"item[0]");
                ProbeProps(item);
                auto s = item.try_as<winrt::Windows::Foundation::IStringable>();
                if (s) Log(L"probe: item[0] ToString='%s'", HStr(s.ToString()).c_str());
            }
        }
    } catch (winrt::hresult_error const& e) {
        Log(L"probe: Items err=0x%08X", (unsigned)e.code());
    } catch (...) {}

    const wchar_t* names[] = {
        L"JumpViewUI.SystemItemViewModel",
        L"JumpViewUI.SystemItemListViewItem",
        L"JumpViewUI.SystemItemControl",
        L"JumpViewUI.SystemItemListView",
    };
    for (auto* name : names) {
        winrt::hstring cls{name};
        IInspectable* raw = nullptr;
        const HRESULT hr = RoActivateInstance(static_cast<HSTRING>(winrt::get_abi(cls)), &raw);
        Log(L"probe: RoActivateInstance(%s) hr=0x%08X raw=%p", name, (unsigned)hr, raw);
        if (raw) {
            winrt::Windows::Foundation::IInspectable inst{raw, winrt::take_ownership_from_abi};
            Log(L"probe:   class=%s", TypeName(inst).c_str());
            LogIids(inst, L"activated");
            ProbeProps(inst);
        }

        IActivationFactory* factory = nullptr;
        const HRESULT fhr = RoGetActivationFactory(
            static_cast<HSTRING>(winrt::get_abi(cls)), IID_IActivationFactory,
            reinterpret_cast<void**>(&factory));
        Log(L"probe: RoGetActivationFactory(%s) hr=0x%08X factory=%p", name,
            (unsigned)fhr, factory);
        if (factory) {
            IInspectable* made = nullptr;
            const HRESULT ahr = factory->ActivateInstance(&made);
            Log(L"probe:   factory ActivateInstance hr=0x%08X obj=%p", (unsigned)ahr, made);
            if (made) {
                winrt::Windows::Foundation::IInspectable m{made, winrt::take_ownership_from_abi};
                Log(L"probe:   made class=%s", TypeName(m).c_str());
                LogIids(m, L"made");
                ProbeProps(m);
            }
            winrt::Windows::Foundation::IInspectable f{factory, winrt::take_ownership_from_abi};
            LogIids(f, L"factory");
        }
    }
}

static Controls::ListViewItem g_ourRow{nullptr};
static HWND g_lastHwnd = nullptr;
// A running menu opened right after a tiny closed menu reuses a window that is
// still ~78 px tall. Inserting our row and resizing in the same beat leaves
// the shell's system block unpainted, so that first growth waits a beat.
static bool g_growPending = false;

static bool PanelHasSystemItem(Controls::Panel const& p) {
    const uint32_t n = p.Children().Size();
    for (uint32_t i = 0; i < n; ++i) {
        auto child = p.Children().GetAt(i);
        auto d = child.try_as<DependencyObject>();
        if (!d) continue;
        const auto aid = HStr(Automation::AutomationProperties::GetAutomationId(d));
        if (aid == L"Close" || aid == L"TaskbarUnpin") return true;
    }
    return false;
}

static void LogPanelChildren(Controls::Panel const& p) {
    const uint32_t n = p.Children().Size();
    if (n > 8) return;
    for (uint32_t i = 0; i < n; ++i) {
        auto ch = p.Children().GetAt(i);
        auto d = ch.try_as<DependencyObject>();
        std::wstring aid, name;
        double h = 0;
        int vis = -1;
        if (auto fe = ch.try_as<FrameworkElement>()) {
            try { name = HStr(fe.Name()); } catch (...) {}
            try { h = fe.ActualHeight(); } catch (...) {}
            try { vis = (int)fe.Visibility(); } catch (...) {}
        }
        if (d) { try { aid = HStr(Automation::AutomationProperties::GetAutomationId(d)); } catch (...) {} }
        Log(L"row: child[%u] %s name='%s' aid='%s' h=%.0f vis=%d",
            i, TypeName(ch).c_str(), name.c_str(), aid.c_str(), h, vis);
    }
}

static Controls::Panel FindItemPanel() {
    // The direct items panel is authoritative when the ListView has already
    // realized its containers; fall back to a visual-tree scan otherwise.
    if (g_sysList) {
        try {
            auto root = g_sysList.ItemsPanelRoot();
            if (root && root.Children().Size() > 0) {
                bool hasLvi = false;
                for (uint32_t i = 0; i < root.Children().Size(); ++i) {
                    if (root.Children().GetAt(i).try_as<Controls::ListViewItem>()) {
                        hasLvi = true;
                        break;
                    }
                }
                if (hasLvi || PanelHasSystemItem(root)) {
                    Log(L"row: panel via ItemsPanelRoot kids=%u lvi=%d",
                        (unsigned)root.Children().Size(), hasLvi ? 1 : 0);
                    LogPanelChildren(root);
                    return root;
                }
            }
        } catch (...) {}
    }

    Controls::Panel best{nullptr};
    int bestCount = 0;
    std::function<void(DependencyObject const&)> walk = [&](DependencyObject const& d) {
        if (!d) return;
        if (auto p = d.try_as<Controls::Panel>()) {
            int lvi = 0;
            const uint32_t n = p.Children().Size();
            for (uint32_t i = 0; i < n; ++i)
                if (p.Children().GetAt(i).try_as<Controls::ListViewItem>()) ++lvi;
            const bool hasSystemItem = PanelHasSystemItem(p);
            // Prefer the panel that owns a native system row (Close/Unpin);
            // otherwise the richest ListViewItem panel.
            if (hasSystemItem || (lvi >= 1 && lvi > bestCount)) {
                best = p;
                bestCount = lvi;
                if (hasSystemItem) return;
            }
        }
        int n = 0;
        try { n = (int)Media::VisualTreeHelper::GetChildrenCount(d); } catch (...) {}
        for (int i = 0; i < n; ++i) {
            try { walk(Media::VisualTreeHelper::GetChild(d, i)); } catch (...) {}
        }
    };
    if (g_sysList) walk(g_sysList);
    return best;
}

static Controls::TextBlock FindDisplayName(Controls::ListViewItem const& item) {
    Controls::TextBlock found{nullptr};
    std::function<void(DependencyObject const&)> walk = [&](DependencyObject const& d) {
        if (!d || found) return;
        if (auto tb = d.try_as<Controls::TextBlock>()) {
            if (HStr(tb.Name()) == L"DisplayNameTextBlock") {
                found = tb;
                return;
            }
        }
        int n = 0;
        try { n = (int)Media::VisualTreeHelper::GetChildrenCount(d); } catch (...) {}
        for (int i = 0; i < n; ++i) {
            try { walk(Media::VisualTreeHelper::GetChild(d, i)); } catch (...) {}
        }
    };
    walk(item);
    return found;
}

static Media::Brush PickForeground(Controls::ListViewItem const& native,
                                   Controls::TextBlock const& nativeText) {
    if (nativeText) {
        auto fg = nativeText.Foreground();
        if (fg) return fg;
    }
    if (native) {
        auto fg = native.Foreground();
        if (fg) return fg;
    }
    try {
        auto res = Application::Current().Resources().Lookup(
            winrt::box_value(L"SystemControlForegroundBaseHighBrush"));
        if (auto b = res.try_as<Media::Brush>()) return b;
    } catch (...) {}

    bool dark = false;
    try { dark = g_sysList && g_sysList.ActualTheme() == ElementTheme::Dark; } catch (...) {}
    if (!dark) {
        try { dark = Application::Current().RequestedTheme() == ApplicationTheme::Dark; } catch (...) {}
    }
    if (!dark) {
        // Last resort: sample the jump-list background luminance.
        try {
            auto win = Window::Current();
            if (win && win.Content()) {
                auto root = win.Content().try_as<Controls::Panel>();
                if (root) {
                    if (auto bg = root.Background().try_as<Media::SolidColorBrush>()) {
                        const auto c = bg.Color();
                        const int lum = (c.R * 299 + c.G * 587 + c.B * 114) / 1000;
                        dark = lum < 128;
                    }
                }
            }
        } catch (...) {}
    }
    return Media::SolidColorBrush{dark ? winrt::Windows::UI::Colors::White()
                                       : winrt::Windows::UI::Colors::Black()};
}

static constexpr wchar_t kOldProcProp[] = L"EndTaskOldProc";

static void LogListState(const wchar_t* tag);
static void LogAncestors(const wchar_t* tag);

// The frame's layout root (the Grid with the task list + system block) is a
// template child, not a ContentControl content. Reach it through the visual
// tree so we can pin/clear its height across opens.
static FrameworkElement FrameLayoutRoot(FrameworkElement const& root) {
    try {
        if (Media::VisualTreeHelper::GetChildrenCount(root) > 0)
            return Media::VisualTreeHelper::GetChild(root, 0).try_as<FrameworkElement>();
    } catch (...) {}
    return nullptr;
}

// Measure the natural height of the jump-list content. The shell reuses the
// same CoreWindow across opens and keeps whatever height it had, so the only
// reliable target is the content's own desired height.
static double MeasureContentDip(FrameworkElement const& root, double widthDip) {
    double need = 0;
    try {
        // The direct layout Grid keeps the previous open's fixed height.
        // Clear it so the star/auto rows report the current app's natural
        // height instead of the last menu's size.
        if (auto content = FrameLayoutRoot(root))
            content.Height(NAN);
        root.Height(NAN); // Auto: let the content dictate its height
        root.Measure(winrt::Windows::Foundation::Size{
            (float)widthDip, std::numeric_limits<float>::infinity()});
        need = root.DesiredSize().Height;
    } catch (...) {}
    return need;
}

static double MeasureTaskListDip(double widthDip) {
    if (!g_sysList) return 0;
    try {
        auto block = Media::VisualTreeHelper::GetParent(g_sysList);
        auto blockGrid = block ? Media::VisualTreeHelper::GetParent(block) : nullptr;
        auto layout = blockGrid ? Media::VisualTreeHelper::GetParent(blockGrid) : nullptr;
        if (auto grid = layout.try_as<Controls::Grid>()) {
            for (uint32_t i = 0; i < grid.Children().Size(); ++i) {
                auto ch = grid.Children().GetAt(i);
                auto fe = ch.try_as<FrameworkElement>();
                if (!fe) continue;
                int r = -1;
                try { r = Controls::Grid::GetRow(fe); } catch (...) {}
                if (r != 0) continue;
                auto sv = ch.try_as<Controls::ScrollViewer>();
                if (!sv || sv.Visibility() != Visibility::Visible) return 0;
                auto content = sv.Content().try_as<FrameworkElement>();
                if (!content) return 0;
                content.Height(NAN);
                content.Measure(winrt::Windows::Foundation::Size{
                    (float)widthDip, std::numeric_limits<float>::infinity()});
                // JumpListControl's desired height already includes the list's
                // padding. Adding 14 here made every menu 14 px taller than
                // the shell's native height (Notepad 584 instead of 570,
                // LibreWolf 232 instead of 218).
                const double h = content.DesiredSize().Height;
                Log(L"fit: task list control=%.0f desired=%.0f -> %.0f",
                    content.ActualHeight(), content.DesiredSize().Height, h);
                return h;
            }
        }
    } catch (...) {}
    return 0;
}

static volatile LONG g_resizeBusy = 0;

struct ResizeRequest {
    HWND hwnd;
    int x, y, w, h;
};

// SetWindowPos from inside our own wndproc makes the shell's XAML root keep
// the old composition size (window grows, content stays clipped). Doing the
// same resize from another thread works, so hand it off to a tiny worker.
static DWORD WINAPI ResizeWorker(LPVOID param) {
    auto* r = static_cast<ResizeRequest*>(param);
    Sleep(50);
    if (IsWindow(r->hwnd)) {
        SetWindowPos(r->hwnd, nullptr, r->x, r->y, r->w, r->h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(r->hwnd, nullptr, TRUE);
        UpdateWindow(r->hwnd);
    }
    delete r;
    InterlockedExchange(&g_resizeBusy, 0);
    return 0;
}

static void RequestResize(HWND h, int x, int y, int w, int hgt) {
    if (InterlockedExchange(&g_resizeBusy, 1) != 0) return;
    auto* req = new ResizeRequest{h, x, y, w, hgt};
    HANDLE th = CreateThread(nullptr, 0, ResizeWorker, req, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete req;
        InterlockedExchange(&g_resizeBusy, 0);
    }
}

static void FitWindowToContent(HWND h, int rows, bool withOurRow = true) {
    if (!g_sysList) return;

    RECT wr{};
    RECT cr{};
    if (!GetWindowRect(h, &wr) || !GetClientRect(h, &cr)) return;
    const int curW = wr.right - wr.left;
    const int curH = wr.bottom - wr.top;
    const int clientH = cr.bottom - cr.top;
    if (curW <= 0 || curH <= 0 || clientH <= 0) return;

    UINT dpi = GetDpiForWindow(h);
    const float scale = dpi > 0 ? dpi / 96.0f : 1.0f;
    const double widthDip = (cr.right - cr.left) / scale;
    const double windowDip = clientH / scale;

    auto win = Window::Current();
    auto root = win ? win.Content().try_as<FrameworkElement>() : nullptr;
    if (!root) return;

    // Our row adds 32 dip to the system block. The task list lives in the
    // star row above it, so in the common case the block can grow upward into
    // that row and the window keeps the shell's native height. Resizing the
    // window past the shell's own height is what left the extra area
    // unpainted (transparent gap, invisible system block).
    const double listDip = MeasureTaskListDip(widthDip);
    const double blockDip = rows * 32.0 + 14.0;
    // What the shell itself would size the window to (without our row).
    const double nativeDip = withOurRow ? blockDip - 32.0 + listDip
                                        : blockDip + listDip;
    const double targetDip = withOurRow
        ? blockDip + listDip - std::min(32.0, listDip)
        : blockDip + listDip;

    const int targetPx = (int)std::ceil(targetDip * scale);
    // Resizing during the early WM_SHOWWINDOW dump happens before the shell
    // has finished positioning the reused CoreWindow for the new app. The
    // window grows but the XAML composition surface stays at the previous
    // app's height (white gap, invisible system block). Wait until the menu
    // is settled (a short timer fires after show) and resize then.
    const bool settled = (GetTickCount64() - g_shownAt) >= 650;
    const bool resized = settled && std::abs(targetPx - curH) > 1;
    if (resized) {
        int newTop = wr.top + curH - targetPx;

        // The shell reuses one CoreWindow for every app and repositions it
        // only after our early WM_SHOWWINDOW dump. Using the stale rect can
        // anchor the menu to the previous app's bottom, so snap to the
        // taskbar edge of the monitor's work area.
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            if (mi.rcWork.bottom < mi.rcMonitor.bottom) {
                newTop = mi.rcWork.bottom - targetPx;   // bottom taskbar
            } else if (mi.rcWork.top > mi.rcMonitor.top) {
                newTop = mi.rcWork.top;                 // top taskbar
            }
        }

        RequestResize(h, wr.left, newTop, curW, targetPx);
    }

    // Pin the frame and its fixed-height ancestors to the height the window
    // will have. Leaving them at the shell's native height clips the extra
    // row; letting them stretch to a stale window height opens the gap.
    try {
        if (std::abs(root.Height() - targetDip) > 0.5) root.Height(targetDip);

        // The frame's direct content is a Grid with a star row (task list)
        // and an auto row (system block). The shell leaves it at the previous
        // open's height, so pin it and top-align it.
        if (auto content = FrameLayoutRoot(root)) {
            const double oldH = content.Height();
            if (std::isnan(oldH) || std::abs(oldH - targetDip) > 0.5) {
                content.Height(targetDip);
                content.VerticalAlignment(VerticalAlignment::Top);
                Log(L"fit: content %s h=%.0f -> %.0f",
                    TypeName(content).c_str(), oldH, targetDip);
            }
        }

        // The frame lives inside a Border + ScrollViewer that the shell also
        // sizes to the native content. Grow/shrink those too or the extra
        // row is clipped by the viewport even though the window is correct.
        auto p = Media::VisualTreeHelper::GetParent(root);
        for (int i = 0; i < 12 && p; ++i) {
            auto fe = p.try_as<FrameworkElement>();
            if (fe) {
                const double fh = fe.Height();
                if (!std::isnan(fh) && std::abs(fh - targetDip) > 0.5) {
                    fe.Height(targetDip);
                    Log(L"fit: ancestor[%d] %s h=%.0f -> %.0f",
                        i, TypeName(p).c_str(), fh, targetDip);
                }
            }
            p = Media::VisualTreeHelper::GetParent(p);
        }

        root.UpdateLayout();
    } catch (...) {}

    LogListState(L"postfit");
    LogAncestors(L"postfit");

    Log(L"row: fit %dx%d -> %dx%d window=%.0f dip list=%.0f native=%.0f target=%.0f settled=%d rows=%d",
        curW, curH, curW, targetPx, windowDip, listDip, nativeDip, targetDip,
        (int)settled, rows);

    if (resized) {
        InvalidateRect(h, nullptr, TRUE);
        UpdateWindow(h);
    }
}

static int CountPanelRows(Controls::Panel const& p) {
    int rows = 0;
    const uint32_t n = p.Children().Size();
    for (uint32_t i = 0; i < n; ++i) {
        auto ch = p.Children().GetAt(i);
        if (ch.try_as<Controls::ListViewItem>()) {
            ++rows;
            continue;
        }
        // Native system rows may use custom containers; count anything laid
        // out at row height so separators/padding don't inflate the block.
        if (auto fe = ch.try_as<FrameworkElement>()) {
            double ah = 0;
            try { ah = fe.ActualHeight(); } catch (...) {}
            if (ah >= 20) ++rows;
        }
    }
    return rows;
}

static int CountSystemRows() {
    if (!g_ourRow) return 0;
    auto rp = Media::VisualTreeHelper::GetParent(g_ourRow);
    auto p = rp.try_as<Controls::Panel>();
    if (!p) return 0;
    return CountPanelRows(p);
}

static void LogListState(const wchar_t* tag) {
    if (!g_sysList) return;
    try {
        Log(L"%s list h=%.0f actual=%.0f max=%.0f min=%.0f vis=%d",
            tag, g_sysList.Height(), g_sysList.ActualHeight(),
            g_sysList.MaxHeight(), g_sysList.MinHeight(),
            (int)g_sysList.Visibility());
        auto panel = FindItemPanel();
        if (panel) {
            Log(L"%s panel h=%.0f actual=%.0f kids=%u",
                tag, panel.Height(), panel.ActualHeight(), panel.Children().Size());
            for (uint32_t i = 0; i < panel.Children().Size(); ++i) {
                auto ch = panel.Children().GetAt(i);
                auto fe = ch.try_as<FrameworkElement>();
                if (!fe) continue;
                double y = -1;
                try {
                    auto pt = fe.TransformToVisual(panel).TransformPoint(
                        winrt::Windows::Foundation::Point{0, 0});
                    y = pt.Y;
                } catch (...) {}
                std::wstring txt;
                int texts = 0;
                int loggedTb = 0;
                std::function<void(DependencyObject const&, int)> tw =
                    [&](DependencyObject const& d, int depth) {
                        if (!d || depth > 6) return;
                        if (auto tb = d.try_as<Controls::TextBlock>()) {
                            ++texts;
                            if (txt.empty()) txt = HStr(tb.Text());
                            if (loggedTb < 4) {
                                double ty = -1;
                                try {
                                    ty = tb.TransformToVisual(fe).TransformPoint(
                                        winrt::Windows::Foundation::Point{0, 0}).Y;
                                } catch (...) {}
                                Log(L"%s     tb[%d] text='%s' y=%.0f w=%.0f h=%.0f vis=%d op=%.2f",
                                    tag, loggedTb, HStr(tb.Text()).c_str(), ty,
                                    tb.ActualWidth(), tb.ActualHeight(),
                                    (int)tb.Visibility(), tb.Opacity());
                                ++loggedTb;
                            }
                        }
                        int n = 0;
                        try { n = (int)Media::VisualTreeHelper::GetChildrenCount(d); } catch (...) {}
                        for (int k = 0; k < n; ++k) {
                            try { tw(Media::VisualTreeHelper::GetChild(d, k), depth + 1); } catch (...) {}
                        }
                    };
                try { tw(ch, 0); } catch (...) {}
                Log(L"%s   kid[%u] %s h=%.0f actual=%.0f y=%.0f text='%s' texts=%d",
                    tag, i, TypeName(ch).c_str(), fe.Height(), fe.ActualHeight(), y,
                    txt.c_str(), texts);
            }
        }
        // The list template's own scroll host: if its viewport is still the
        // native row count, the last row gets clipped regardless of Height.
        std::function<void(DependencyObject const&, int)> walk =
            [&](DependencyObject const& d, int depth) {
                if (!d || depth > 6) return;
                if (auto sv = d.try_as<Controls::ScrollViewer>()) {
                    Log(L"%s scrollviewer actual=%.0f viewport=%.0f extent=%.0f scrollable=%.0f",
                        tag, sv.ActualHeight(), sv.ViewportHeight(),
                        sv.ExtentHeight(), sv.ScrollableHeight());
                }
                int n = 0;
                try { n = (int)Media::VisualTreeHelper::GetChildrenCount(d); } catch (...) {}
                for (int i = 0; i < n; ++i) {
                    try { walk(Media::VisualTreeHelper::GetChild(d, i), depth + 1); } catch (...) {}
                }
            };
        walk(g_sysList, 0);
    } catch (...) {}
}

static void LogAncestors(const wchar_t* tag) {
    try {
        auto win = Window::Current();
        auto root = win ? win.Content().try_as<FrameworkElement>() : nullptr;
        if (!root) return;
        Log(L"%s root h=%.0f actual=%.0f", tag, root.Height(), root.ActualHeight());
        auto p = Media::VisualTreeHelper::GetParent(root);
        for (int i = 0; i < 8 && p; ++i) {
            auto fe = p.try_as<FrameworkElement>();
            if (fe) {
                Log(L"%s anc[%d] %s h=%.0f actual=%.0f max=%.0f",
                    tag, i, TypeName(p).c_str(), fe.Height(),
                    fe.ActualHeight(), fe.MaxHeight());
            } else {
                Log(L"%s anc[%d] %s", tag, i, TypeName(p).c_str());
            }
            p = Media::VisualTreeHelper::GetParent(p);
        }

        if (g_sysList) {
            DependencyObject d = g_sysList;
            for (int i = 0; i < 10 && d; ++i) {
                auto fe = d.try_as<FrameworkElement>();
                double y = -1, ah = -1, fh = -1;
                int vis = -1;
                double op = -1;
                if (fe) {
                    try {
                        y = fe.TransformToVisual(root).TransformPoint(
                            winrt::Windows::Foundation::Point{0, 0}).Y;
                    } catch (...) {}
                    ah = fe.ActualHeight();
                    fh = fe.Height();
                    vis = (int)fe.Visibility();
                    op = fe.Opacity();
                }
                Log(L"%s chain[%d] %s h=%.0f actual=%.0f y=%.0f vis=%d op=%.2f",
                    tag, i, TypeName(d).c_str(), fh, ah, y, vis, op);
                d = Media::VisualTreeHelper::GetParent(d);
            }
        }

        int n = 0;
        try { n = (int)Media::VisualTreeHelper::GetChildrenCount(root); } catch (...) {}
        Log(L"%s root children=%d", tag, n);
        for (int i = 0; i < n && i < 12; ++i) {
            auto ch = Media::VisualTreeHelper::GetChild(root, i);
            auto fe = ch.try_as<FrameworkElement>();
            if (!fe) {
                Log(L"%s   rkid[%d] %s", tag, i, TypeName(ch).c_str());
                continue;
            }
            double y = -1;
            try {
                y = fe.TransformToVisual(root).TransformPoint(
                    winrt::Windows::Foundation::Point{0, 0}).Y;
            } catch (...) {}
            Log(L"%s   rkid[%d] %s h=%.0f actual=%.0f y=%.0f vis=%d",
                tag, i, TypeName(ch).c_str(), fe.Height(), fe.ActualHeight(),
                y, (int)fe.Visibility());
        }

        // The frame's direct child carries the real layout: tasks list in a
        // star row, system block in an auto row. Log its rows so a stale
        // height from the previous open is obvious.
        auto first = n > 0 ? Media::VisualTreeHelper::GetChild(root, 0) : nullptr;
        if (auto grid = first.try_as<Controls::Grid>()) {
            const uint32_t rows = grid.RowDefinitions().Size();
            Log(L"%s grid rows=%u kids=%u actual=%.0f h=%.0f min=%.0f",
                tag, rows, grid.Children().Size(), grid.ActualHeight(),
                grid.Height(), grid.MinHeight());
            for (uint32_t r = 0; r < rows; ++r) {
                auto rd = grid.RowDefinitions().GetAt(r);
                Log(L"%s   gridrow[%u] value=%.0f unit=%d",
                    tag, r, rd.Height().Value, (int)rd.Height().GridUnitType);
            }
            for (uint32_t i = 0; i < grid.Children().Size() && i < 12; ++i) {
                auto ch = grid.Children().GetAt(i);
                auto fe = ch.try_as<FrameworkElement>();
                if (!fe) continue;
                double y = -1;
                try {
                    y = fe.TransformToVisual(root).TransformPoint(
                        winrt::Windows::Foundation::Point{0, 0}).Y;
                } catch (...) {}
                int gr = -1;
                try { gr = Controls::Grid::GetRow(fe); } catch (...) {}
                Log(L"%s   gkid[%u] %s row=%d h=%.0f actual=%.0f y=%.0f vis=%d",
                    tag, i, TypeName(ch).c_str(), gr, fe.Height(),
                    fe.ActualHeight(), y, (int)fe.Visibility());
                if (auto sv = ch.try_as<Controls::ScrollViewer>()) {
                    auto sc = sv.Content().try_as<FrameworkElement>();
                    Log(L"%s     scroll extent=%.0f viewport=%.0f content=%s ch=%.0f ca=%.0f",
                        tag, sv.ExtentHeight(), sv.ViewportHeight(),
                        sc ? TypeName(sc).c_str() : L"<null>",
                        sc ? sc.Height() : -1.0, sc ? sc.ActualHeight() : -1.0);
                }
            }
        }
    } catch (...) {}
}

static void GrowSystemArea(HWND h) {
    if (!g_sysList) return;

    int rows = CountSystemRows();
    if (rows <= 0) rows = 3;
    const double listH = rows * 32.0;
    const double borderH = listH + 14.0;

    if (g_sysList.Height() != listH) g_sysList.Height(listH);

    auto parent = Media::VisualTreeHelper::GetParent(g_sysList);
    auto border = parent ? parent.try_as<Controls::Border>() : nullptr;
    if (border) {
        if (border.Height() != borderH) border.Height(borderH);
        if (border.MinHeight() != borderH) border.MinHeight(borderH);
    }

    // A tall task list can paint past its ScrollViewer viewport and cover
    // the system block. Keep the block above it in the grid's z-order.
    auto block = parent ? Media::VisualTreeHelper::GetParent(parent) : nullptr;
    if (auto blockFe = block.try_as<FrameworkElement>()) {
        if (Controls::Canvas::GetZIndex(blockFe) < 10) {
            Controls::Canvas::SetZIndex(blockFe, 10);
            Log(L"fit: system block z=10 (%s)", TypeName(block).c_str());
        }
    }

    try { g_sysList.UpdateLayout(); } catch (...) {}
    if (border) { try { border.UpdateLayout(); } catch (...) {} }

    FitWindowToContent(h, rows);
}

static void WriteEndTaskRequest(HWND h);
static void TriggerEndTask(HWND h);

static void OnRowTapped(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs const&) {
    // Keyboard activation path (Enter/Space). Pointer input is intercepted in
    // the wndproc before XAML sees it; this stays as a fallback and shares the
    // same debounce.
    HWND h = g_lastHwnd;
    if (h) TriggerEndTask(h);
}

static void WriteEndTaskRequest(HWND h) {
    wchar_t title[256] = {};
    GetWindowTextW(h, title, 256);
    HANDLE f = CreateFileW(L"C:\\ProgramData\\EndTaskMenu\\request.txt",
                           GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log(L"tap: request write failed err=%lu", GetLastError());
        return;
    }
    DWORD written = 0;
    DWORD bytes = (DWORD)(wcslen(title) * sizeof(wchar_t));
    WriteFile(f, title, bytes, &written, nullptr);
    CloseHandle(f);
    Log(L"tap: request '%s'", title);
}

static bool ContainsOurText(DependencyObject const& d) {
    bool found = false;
    std::function<void(DependencyObject const&)> walk = [&](DependencyObject const& n) {
        if (!n || found) return;
        if (auto tb = n.try_as<Controls::TextBlock>()) {
            try {
                if (HStr(tb.Text()) == L"End task") {
                    found = true;
                    return;
                }
            } catch (...) {}
        }
        int count = 0;
        try { count = (int)Media::VisualTreeHelper::GetChildrenCount(n); } catch (...) {}
        for (int i = 0; i < count; ++i) {
            try { walk(Media::VisualTreeHelper::GetChild(n, i)); } catch (...) {}
        }
    };
    walk(d);
    return found;
}

static void EnsureEndTaskRow(HWND h) {
    g_lastHwnd = h;
    if (!g_sysList) return;
    if (!IsWindowVisible(h)) return;

    auto panel = FindItemPanel();
    if (!panel) {
        Log(L"row: no item panel");
        return;
    }

    // The row only makes sense when the app is running. A running app always
    // has the native "Close window" row; a closed/pinned app has none. Detect
    // that first so closed menus never get an "End task" row.
    uint32_t closeIndex = UINT32_MAX;
    Controls::ListViewItem native{nullptr};
    for (uint32_t i = 0; i < panel.Children().Size(); ++i) {
        auto child = panel.Children().GetAt(i);
        if (auto lvi = child.try_as<Controls::ListViewItem>()) {
            if (!native) native = lvi;
            auto d = child.try_as<DependencyObject>();
            if (d && HStr(Automation::AutomationProperties::GetAutomationId(d)) == L"Close")
                closeIndex = i;
        }
    }
    if (closeIndex == UINT32_MAX) {
        Log(L"row: skipped, app closed (no Close row)");
        // Remove a row left over from an earlier template, if any.
        for (uint32_t i = 0; i < panel.Children().Size(); ) {
            auto child = panel.Children().GetAt(i);
            auto lvi = child.try_as<Controls::ListViewItem>();
            auto d = child.try_as<DependencyObject>();
            const auto aid = d ? HStr(Automation::AutomationProperties::GetAutomationId(d))
                               : std::wstring();
            if (lvi && d && (aid == L"EndTask" || ContainsOurText(d))) {
                panel.Children().RemoveAt(i);
                Log(L"row: removed stale EndTask row at %u", i);
                continue;
            }
            ++i;
        }
        g_ourRow = nullptr;
        g_growPending = false;
        KillTimer(h, kGrowTimer);
        // Closed app: no EndTask row, but the reused CoreWindow still carries
        // the previous menu's sizes and our SystemItemList height. Restore the
        // block to its native row count first: the Tasks/Recent entries live
        // in the star row above it and must not be clipped by the old pin.
        const int nativeRows = CountPanelRows(panel);
        if (nativeRows > 0) {
            const double listH = nativeRows * 32.0;
            try {
                g_sysList.Height(listH);
                auto border = Media::VisualTreeHelper::GetParent(g_sysList)
                                  .try_as<FrameworkElement>();
                if (border) {
                    border.Height(listH + 14.0);
                    border.MinHeight(listH + 14.0);
                }
            } catch (...) {}
            FitWindowToContent(h, nativeRows, false);
        }
        return;
    }

    // Adopt an existing EndTask row instead of stacking duplicates. This
    // also covers a second hook copy or a panel that survived a re-template.
    for (uint32_t i = 0; i < panel.Children().Size(); ++i) {
        auto child = panel.Children().GetAt(i);
        auto lvi = child.try_as<Controls::ListViewItem>();
        if (!lvi) continue;
        auto d = child.try_as<DependencyObject>();
        if (!d) continue;
        const auto aid = HStr(Automation::AutomationProperties::GetAutomationId(d));
        if (aid == L"EndTask" || ContainsOurText(d)) {
            if (g_ourRow != lvi) {
                Log(L"row: adopting existing EndTask row at %u", i);
                g_ourRow = lvi;
            }
            try {
                Automation::AutomationProperties::SetAutomationId(lvi, L"EndTask");
            } catch (...) {}
            try { g_ourRow.Opacity(1.0); } catch (...) {}
            try { g_ourRow.Visibility(Visibility::Visible); } catch (...) {}
            if (!g_growPending) GrowSystemArea(h);
            return;
        }
    }
    g_ourRow = nullptr;

    // Insert directly above the native "Close window" row.
    const uint32_t insertIndex = closeIndex;

    auto nativeText = FindDisplayName(native);
    auto fg = PickForeground(native, nativeText);

    Controls::ListViewItem row;
    row.Height(32);
    row.MinHeight(0);
    row.Padding(Thickness{0});
    row.Margin(Thickness{0});
    row.BorderThickness(Thickness{0});
    row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    row.VerticalContentAlignment(VerticalAlignment::Center);
    row.IsTabStop(false);
    row.Opacity(1.0);
    row.Visibility(Visibility::Visible);
    row.Background(Media::SolidColorBrush{winrt::Windows::UI::Colors::Transparent()});
    row.Foreground(fg);
    if (nativeText) {
        if (nativeText.FontSize() > 0) row.FontSize(nativeText.FontSize());
        if (nativeText.FontFamily()) row.FontFamily(nativeText.FontFamily());
    }

    Controls::Grid content;
    content.Height(30);
    content.Margin(Thickness{1, 1, 1, 1});

    Controls::ColumnDefinition col0;
    col0.Width(GridLength{41, GridUnitType::Pixel});
    Controls::ColumnDefinition col1;
    col1.Width(GridLength{1, GridUnitType::Star});
    content.ColumnDefinitions().Append(col0);
    content.ColumnDefinitions().Append(col1);

    // Circle with a diagonal bar, drawn from primitives so it renders the
    // same on every Windows 10 build regardless of icon-font coverage.
    Controls::Grid icon;
    icon.Width(16);
    icon.Height(16);
    icon.HorizontalAlignment(HorizontalAlignment::Left);
    icon.VerticalAlignment(VerticalAlignment::Center);
    icon.Margin(Thickness{12, 0, 0, 0});

    Shapes::Ellipse circle;
    circle.Width(14);
    circle.Height(14);
    circle.Stroke(fg);
    circle.StrokeThickness(1.4);
    circle.HorizontalAlignment(HorizontalAlignment::Center);
    circle.VerticalAlignment(VerticalAlignment::Center);
    icon.Children().Append(circle);

    Shapes::Line slash;
    slash.X1(3.6);
    slash.Y1(3.6);
    slash.X2(12.4);
    slash.Y2(12.4);
    slash.Stroke(fg);
    slash.StrokeThickness(1.4);
    icon.Children().Append(slash);

    content.Children().Append(icon);

    Controls::TextBlock text;
    text.Text(L"End task");
    text.VerticalAlignment(VerticalAlignment::Center);
    text.TextTrimming(TextTrimming::CharacterEllipsis);
    text.Foreground(fg);
    if (nativeText) {
        if (nativeText.FontSize() > 0) text.FontSize(nativeText.FontSize());
        if (nativeText.FontFamily()) text.FontFamily(nativeText.FontFamily());
        text.FontWeight(nativeText.FontWeight());
    } else {
        text.FontSize(12);
    }
    Controls::Grid::SetColumn(text, 1);
    content.Children().Append(text);

    row.Content(content);
    Automation::AutomationProperties::SetAutomationId(row, L"EndTask");
    Automation::AutomationProperties::SetName(row, L"End task");

    // Tap detected via WatchThread polling + WndProc WM_HOOK_DUMP wp=2

    panel.Children().InsertAt(insertIndex, row);
    g_ourRow = row;
    Log(L"row: inserted at %u, panel kids=%u", insertIndex,
        (unsigned)panel.Children().Size());

    try { panel.UpdateLayout(); } catch (...) {}
    try { g_sysList.UpdateLayout(); } catch (...) {}
    RECT rowClient{};
    GetClientRect(h, &rowClient);
    if (rowClient.bottom - rowClient.top < 100) {
        g_growPending = true;
        SetTimer(h, kGrowTimer, 350, nullptr);
        Log(L"row: grow deferred (window=%d px)",
            (int)(rowClient.bottom - rowClient.top));
    } else {
        g_growPending = false;
        GrowSystemArea(h);
    }
    try { g_sysList.UpdateLayout(); } catch (...) {}
    Log(L"row: sized list h=%.0f row h=%.0f vis=%d op=%.2f",
        g_sysList.ActualHeight(), row.ActualHeight(),
        (int)row.Visibility(), row.Opacity());
}

static void DumpElement(DependencyObject const& d, int depth, int& budget) {
    if (!d || depth > 40 || budget-- <= 0) return;

    std::wstring type = L"<non-winrt>";
    try { type = HStr(winrt::get_class_name(d)); } catch (...) {}

    std::wstring name, aid;
    double x = 0, y = 0, w = 0, h = 0;
    auto fe = d.try_as<FrameworkElement>();
    if (fe) {
        try { name = HStr(fe.Name()); } catch (...) {}
        try { w = fe.ActualWidth(); h = fe.ActualHeight(); } catch (...) {}
        try {
            auto t = fe.TransformToVisual(nullptr);
            auto pt = t.TransformPoint(winrt::Windows::Foundation::Point{0, 0});
            x = pt.X;
            y = pt.Y;
        } catch (...) {}
    }

    try { aid = HStr(Automation::AutomationProperties::GetAutomationId(d)); } catch (...) {}

    if (aid == L"SystemItemList") {
        if (auto lv = d.try_as<Controls::ListView>()) g_sysList = lv;
    }

    int children = 0;
    try { children = (int)Media::VisualTreeHelper::GetChildrenCount(d); } catch (...) {}

    Log(L"%*s%s name='%s' aid='%s' pos=%.0f,%.0f size=%.0fx%.0f kids=%d",
        depth * 2, L"", type.c_str(), name.c_str(), aid.c_str(),
        x, y, w, h, children);

    if (auto ic = d.try_as<Controls::ItemsControl>()) {
        try {
            auto items = ic.Items();
            auto src = ic.ItemsSource();
            Log(L"%*s  items=%u source=%s", depth * 2, L"",
                (unsigned)items.Size(), TypeName(src).c_str());
        } catch (winrt::hresult_error const& e) {
            Log(L"%*s  items err=0x%08X", depth * 2, L"", (unsigned)e.code());
        } catch (...) {
            Log(L"%*s  items err=<unknown>", depth * 2, L"");
        }
    }

    if (auto lvi = d.try_as<Controls::ListViewItem>()) {
        try {
            auto content = lvi.Content();
            Log(L"%*s  content=%s", depth * 2, L"", TypeName(content).c_str());
        } catch (...) {}
    }

    for (int i = 0; i < children && budget > 0; ++i) {
        try {
            auto child = Media::VisualTreeHelper::GetChild(d, i);
            if (child) DumpElement(child, depth + 1, budget);
        } catch (...) {}
    }
}

static bool PointInRow(winrt::Windows::Foundation::Point const& pt) {
    if (!g_ourRow) return false;
    try {
        auto win = Window::Current();
        if (!win || !win.Content()) return false;
        auto root = win.Content();
        auto t = g_ourRow.TransformToVisual(root);
        auto topLeft = t.TransformPoint(winrt::Windows::Foundation::Point{0, 0});
        auto w = (float)g_ourRow.ActualWidth();
        auto hgt = (float)g_ourRow.ActualHeight();
        return pt.X >= topLeft.X && pt.X <= topLeft.X + w &&
               pt.Y >= topLeft.Y && pt.Y <= topLeft.Y + hgt;
    } catch (...) {}
    return false;
}

static ULONGLONG g_lastTap = 0;
static bool g_rowPressed = false;

// The inserted ListViewItem is not part of the ListView's ItemsSource, so the
// control's native hit-testing is off by one: a click on our visually correct
// row activates the native Close row underneath. Swallow the press/release in
// the CoreWindow wndproc and trigger the kill ourselves so XAML never sees it.
static bool ScreenPointInRow(HWND h) {
    if (!g_ourRow) return false;
    try {
        POINT screen{};
        if (!GetCursorPos(&screen)) return false;
        ScreenToClient(h, &screen);
        const UINT dpi = GetDpiForWindow(h);
        const float scale = dpi > 0 ? dpi / 96.0f : 1.0f;
        return PointInRow(winrt::Windows::Foundation::Point{
            screen.x / scale, screen.y / scale});
    } catch (...) {}
    return false;
}

static void TriggerEndTask(HWND h) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastTap < 600) return;
    g_lastTap = now;
    Log(L"row: end-task");
    WriteEndTaskRequest(h);
    // Do NOT post WM_CLOSE to the flyout: the shell forwards that close to
    // the app whose jump list is open (Notepad/Explorer died on every test).
    // The elevated helper kills the target from request.txt and the shell
    // dismisses the flyout by itself when the app goes away.
    //
    // Closed apps (or targets the helper cannot resolve) never go away, so
    // the flyout would stay open forever. Give the helper a moment to kill,
    // then close the flyout window ourselves if it is still visible.
    SetTimer(h, kDismissTimer, 400, nullptr);
}

static bool FindSystemListQuiet(DependencyObject const& d, int depth, int& budget) {
    if (!d || budget-- <= 0) return false;
    try {
        auto aid = Automation::AutomationProperties::GetAutomationId(d);
        if (HStr(aid) == L"SystemItemList") {
            if (auto lv = d.try_as<Controls::ListView>()) {
                g_sysList = lv;
                return true;
            }
        }
    } catch (...) {}

    int children = 0;
    try { children = (int)Media::VisualTreeHelper::GetChildrenCount(d); } catch (...) {}
    for (int i = 0; i < children && budget > 0; ++i) {
        try {
            auto child = Media::VisualTreeHelper::GetChild(d, i);
            if (child && FindSystemListQuiet(child, depth + 1, budget)) return true;
        } catch (...) {}
    }
    return false;
}

static void DoDump(HWND h) {
    HRESULT hr = RoInitialize(RO_INIT_SINGLETHREADED);
    const bool uninit = SUCCEEDED(hr);
    g_sysList = nullptr;

    try {
        auto win = Window::Current();
        if (win) {
            auto content = win.Content();
            if (content) {
                int budget = 4000;
                FindSystemListQuiet(content, 0, budget);
                EnsureEndTaskRow(h);
                LogListState(L"dump");
                LogAncestors(L"dump");
            }
        }
    } catch (...) {}

    if (uninit) RoUninitialize();
}

static void RemoveTracked(HWND h) {
    AcquireSRWLockExclusive(&g_lock);
    g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), h), g_windows.end());
    ReleaseSRWLockExclusive(&g_lock);
}

static LRESULT CALLBACK JumpWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC old = (WNDPROC)GetPropW(h, kOldProcProp);
    switch (msg) {
    case WM_HOOK_DUMP: {
        const ULONGLONG now = GetTickCount64();
        const DWORD minGap = (wp == 1) ? 0 : 200;
        if (now - g_lastDump >= minGap) {
            g_lastDump = now;
            DoDump(h);
        }
        return 0;
    }
    case WM_SHOWWINDOW:
        if (wp) {
            g_shownAt = GetTickCount64();
            PostMessageW(h, WM_HOOK_DUMP, 1, 0);
            SetTimer(h, kFitTimer, 700, nullptr);
        } else {
            KillTimer(h, kFitTimer);
            KillTimer(h, kDismissTimer);
            KillTimer(h, kGrowTimer);
            g_growPending = false;
            // Drop our height override so the next open starts from the
            // shell's own layout instead of the previous menu's size.
            try {
                auto win = Window::Current();
                if (win && win.Content()) {
                    if (auto root = win.Content().try_as<FrameworkElement>()) {
                        root.Height(NAN);
                        if (auto content = FrameLayoutRoot(root))
                            content.Height(NAN);
                    }
                }
            } catch (...) {}
        }
        break;
    case WM_ACTIVATE:
    case WM_WINDOWPOSCHANGED:
        PostMessageW(h, WM_HOOK_DUMP, 1, 0);
        break;
    case WM_LBUTTONDOWN:
    case 0x0246: // WM_POINTERDOWN
        // Some precision touchpads deliver WM_POINTERDOWN without
        // POINTER_FLAG_FIRSTBUTTON in wParam (same as WM_POINTERUP). Trust
        // the hit-test instead of the flag, otherwise the press reaches XAML
        // and the release fires nothing.
        if (ScreenPointInRow(h)) {
            g_rowPressed = true;
            Log(L"row: press swallowed msg=0x%X wp=0x%llX", msg, (unsigned long long)wp);
            return 0;
        }
        g_rowPressed = false;
        break;
    case WM_LBUTTONUP:
    case 0x0247: // WM_POINTERUP
    {
        // Pointer-up wParam does not carry POINTER_FLAG_FIRSTBUTTON (the
        // button is reported as ButtonChangeType instead), so do not gate the
        // release on that flag: if we swallowed the press, or the cursor is
        // still over our row, consume the release.
        const bool inRow = ScreenPointInRow(h);
        if (msg == WM_LBUTTONUP || g_rowPressed || inRow) {
            const bool fire = g_rowPressed && inRow;
            g_rowPressed = false;
            Log(L"row: release swallowed inRow=%d fire=%d msg=0x%X", (int)inRow, (int)fire, msg);
            if (fire) TriggerEndTask(h);
            return 0;
        }
        break;
    }
    case WM_PAINT:
        if (GetTickCount64() - g_lastDump >= 500)
            PostMessageW(h, WM_HOOK_DUMP, 0, 0);
        break;
    case WM_TIMER:
        if (wp == kFitTimer) {
            KillTimer(h, kFitTimer);
            PostMessageW(h, WM_HOOK_DUMP, 1, 0);
            return 0;
        }
        if (wp == kGrowTimer) {
            KillTimer(h, kGrowTimer);
            if (g_growPending && g_ourRow && g_sysList) {
                g_growPending = false;
                Log(L"row: applying deferred grow");
                GrowSystemArea(h);
                try { g_sysList.UpdateLayout(); } catch (...) {}
            }
            return 0;
        }
        if (wp == kDismissTimer) {
            KillTimer(h, kDismissTimer);
            if (IsWindowVisible(h)) {
                Log(L"dismiss: still open, closing flyout");
                PostMessageW(h, WM_HOOK_DISMISS, 0, 0);
            }
            return 0;
        }
        break;
    case WM_HOOK_DISMISS: {
        // Window.Close() on the jump list CoreWindow takes the whole
        // ShellExperienceHost down with it, so dismiss with a real Escape
        // instead (same as the user pressing Esc). Only do it while the
        // flyout owns the foreground: if the target already died and the
        // flyout is on its way out, the Escape would land on another window.
        HWND fg = GetForegroundWindow();
        if (fg == h) {
            Log(L"dismiss: sending Escape");
            keybd_event(VK_ESCAPE, 0, 0, 0);
            keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
        } else {
            Log(L"dismiss: skipped (fg=%p h=%p)", (void*)fg, (void*)h);
        }
        return 0;
    }
    case WM_NCDESTROY: {
        RemoveTracked(h);
        g_sysList = nullptr;
        g_ourRow = nullptr;
        g_growPending = false;
        Log(L"window %p destroyed", (void*)h);
        if (old) {
            SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)old);
            RemovePropW(h, kOldProcProp);
        }
        return old ? CallWindowProcW(old, h, msg, wp, lp)
                   : DefWindowProcW(h, msg, wp, lp);
    }
    }
    return old ? CallWindowProcW(old, h, msg, wp, lp)
               : DefWindowProcW(h, msg, wp, lp);
}

static void RegisterWindow(HWND h) {
    bool added = false;
    AcquireSRWLockExclusive(&g_lock);
    if (std::find(g_windows.begin(), g_windows.end(), h) == g_windows.end()) {
        g_windows.push_back(h);
        added = true;
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (added) {
        Log(L"found jump list hwnd=%p", (void*)h);
        SetLastError(0);
        const LONG_PTR prev = SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)JumpWndProc);
        Log(L"SetWindowLongPtr(%p) -> prev=%p err=%lu", (void*)h,
            (void*)prev, GetLastError());
        if (prev) {
            SetPropW(h, kOldProcProp, (HANDLE)(WNDPROC)prev);
            PostMessageW(h, WM_HOOK_DUMP, 1, 0);
        }
    }
}

struct FindCtx {
    HWND found;
};

static BOOL CALLBACK ThreadWndProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    wchar_t cls[128] = {};
    if (!GetClassNameW(h, cls, 128)) return TRUE;
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow")) return TRUE;
    wchar_t title[256] = {};
    GetWindowTextW(h, title, 256);
    if (wcsncmp(title, L"Jump List", 9)) return TRUE;
    c->found = h;
    return FALSE;
}

static void CheckWindow(HWND h) {
    if (!h) return;
    wchar_t cls[128] = {};
    if (!GetClassNameW(h, cls, 128)) return;
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow")) return;
    wchar_t title[256] = {};
    GetWindowTextW(h, title, 256);
    if (wcsncmp(title, L"Jump List", 9)) return;
    RegisterWindow(h);
}

static DWORD WINAPI WatchThread(LPVOID) {
    Log(L"watch thread started (pid=%lu)", GetCurrentProcessId());

    bool wasDown = false;
    ULONGLONG lastScan = 0;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now - lastScan >= 500) {
            lastScan = now;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                if (Thread32First(snap, &te)) {
                    do {
                        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                        FindCtx ctx{nullptr};
                        EnumThreadWindows(te.th32ThreadID, ThreadWndProc, (LPARAM)&ctx);
                        if (ctx.found) RegisterWindow(ctx.found);
                    } while (Thread32Next(snap, &te));
                }
                CloseHandle(snap);
            }
            CheckWindow(GetForegroundWindow());
        }

        HWND fg = GetForegroundWindow();
        bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (down && !wasDown) {
            AcquireSRWLockShared(&g_lock);
            for (HWND w : g_windows) {
                if (w == fg) PostMessageW(w, WM_HOOK_DUMP, 2, 0);
            }
            ReleaseSRWLockShared(&g_lock);
        }
        wasDown = down;
        Sleep(30);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Log(L"DllMain attach");
        HANDLE t = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
