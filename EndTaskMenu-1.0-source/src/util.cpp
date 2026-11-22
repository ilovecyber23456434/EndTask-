#include "util.h"
#include <cwctype>
#include <dwmapi.h>
#include <propkey.h>
#include <propsys.h>
#include <shobjidl.h>
#include <shlobj.h>

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

bool IEquals(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool IContains(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    return ToLower(hay).find(ToLower(needle)) != std::wstring::npos;
}

bool StartsWithI(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    return CompareStringOrdinal(s.c_str(), (int)prefix.size(), prefix.c_str(), (int)prefix.size(), TRUE) == CSTR_EQUAL;
}

std::wstring FileNameOf(const std::wstring& path) {
    auto p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring ProcessImagePath(DWORD pid) {
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return out;
    wchar_t buf[1024];
    DWORD n = (DWORD)std::size(buf);
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

std::wstring GetWindowAumid(HWND hwnd) {
    std::wstring out;
    IPropertyStore* ps = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&ps))) || !ps) return out;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv))) {
        if (pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
    }
    PropVariantClear(&pv);
    ps->Release();
    return out;
}

bool IsCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != FALSE;
    return false;
}
