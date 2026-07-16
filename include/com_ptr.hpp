#pragma once

#include <windows.h>
#include <objbase.h>

#include <utility>

template <typename T>
class ComPtr {
public:
    ComPtr() = default;

    explicit ComPtr(T* ptr) noexcept : ptr_(ptr) {}

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    ~ComPtr() {
        reset();
    }

    T* get() const noexcept {
        return ptr_;
    }

    T** put() noexcept {
        reset();
        return &ptr_;
    }

    T* operator->() const noexcept {
        return ptr_;
    }

    explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }

    void reset(T* ptr = nullptr) noexcept {
        if (ptr_) {
            ptr_->Release();
        }
        ptr_ = ptr;
    }

    T* detach() noexcept {
        return std::exchange(ptr_, nullptr);
    }

private:
    T* ptr_ = nullptr;
};

class ComInitializer {
public:
    explicit ComInitializer(DWORD flags = COINIT_APARTMENTTHREADED)
        : hr_(CoInitializeEx(nullptr, flags)) {}

    ~ComInitializer() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }

    ComInitializer(const ComInitializer&) = delete;
    ComInitializer& operator=(const ComInitializer&) = delete;

    [[nodiscard]] bool ok() const noexcept {
        return SUCCEEDED(hr_) || hr_ == S_FALSE || hr_ == RPC_E_CHANGED_MODE;
    }

    [[nodiscard]] HRESULT result() const noexcept {
        return hr_;
    }

private:
    HRESULT hr_;
};

class BStr {
public:
    BStr() = default;

    explicit BStr(const wchar_t* value)
        : value_(SysAllocString(value)) {}

    ~BStr() {
        if (value_) {
            SysFreeString(value_);
        }
    }

    BStr(const BStr&) = delete;
    BStr& operator=(const BStr&) = delete;

    BStr(BStr&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }

    BStr& operator=(BStr&& other) noexcept {
        if (this != &other) {
            if (value_) {
                SysFreeString(value_);
            }
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] BSTR get() const noexcept {
        return value_;
    }

    BSTR* put() noexcept {
        if (value_) {
            SysFreeString(value_);
            value_ = nullptr;
        }
        return &value_;
    }

    operator BSTR() const noexcept {
        return value_;
    }

    explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

private:
    BSTR value_ = nullptr;
};
