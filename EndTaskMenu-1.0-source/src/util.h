#pragma once
#include <windows.h>
#include <string>

template <class T>
class ComPtr {
    T* p_ = nullptr;
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    T** put() { reset(); return &p_; }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    void attach(T* q) { reset(); p_ = q; }
};

std::wstring ToLower(std::wstring s);
bool IEquals(const std::wstring& a, const std::wstring& b);
bool IContains(const std::wstring& hay, const std::wstring& needle);
bool StartsWithI(const std::wstring& s, const std::wstring& prefix);
std::wstring FileNameOf(const std::wstring& path);
std::wstring ProcessImagePath(DWORD pid);
std::wstring GetWindowAumid(HWND hwnd);
bool IsCloaked(HWND hwnd);
