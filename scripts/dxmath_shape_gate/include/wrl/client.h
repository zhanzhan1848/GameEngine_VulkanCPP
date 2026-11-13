#pragma once
#include "ole2.h"  // IUnknown/IID via shim
#include <utility>
#include <memory>
#include <type_traits>
namespace Microsoft { namespace WRL {
// Minimal ComPtr for syntax gating (ref-counted COM pointer).
template <typename T>
class ComPtr {
public:
    typedef T InterfaceType;
    ComPtr() noexcept : ptr_(nullptr) {}
    ComPtr(std::nullptr_t) noexcept : ptr_(nullptr) {}
    ComPtr(T* other) noexcept : ptr_(other) { InternalAddRef(); }
    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) { InternalAddRef(); }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ~ComPtr() { InternalRelease(); }
    ComPtr& operator=(std::nullptr_t) { InternalRelease(); ptr_ = nullptr; return *this; }
    ComPtr& operator=(T* other) noexcept { if (ptr_ != other) { InternalAddRefOther(other); InternalRelease(); ptr_ = other; } return *this; }
    ComPtr& operator=(const ComPtr& other) noexcept {
        if (this != std::addressof(other)) { InternalAddRefOther(other.ptr_); InternalRelease(); ptr_ = other.ptr_; }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != std::addressof(other)) { InternalRelease(); ptr_ = other.ptr_; other.ptr_ = nullptr; }
        return *this;
    }
    T* operator->() const noexcept { return ptr_; }
    T* Get() const noexcept { return ptr_; }
    T* Detach() noexcept { T* p = ptr_; ptr_ = nullptr; return p; }
    void Reset() noexcept { InternalRelease(); ptr_ = nullptr; }
    T** operator&() noexcept { InternalRelease(); ptr_ = nullptr; return &ptr_; }
    T** GetAddressOf() noexcept { return &ptr_; }
    T** ReleaseAndGetAddressOf() noexcept { InternalRelease(); ptr_ = nullptr; return &ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    void CopyTo(T** pp) const noexcept { InternalAddRef(); *pp = ptr_; }
    void Swap(ComPtr& o) noexcept { T* t = ptr_; ptr_ = o.ptr_; o.ptr_ = t; }
    friend bool operator==(const ComPtr& a, const ComPtr& b) noexcept { return a.ptr_ == b.ptr_; }
    friend bool operator!=(const ComPtr& a, const ComPtr& b) noexcept { return a.ptr_ != b.ptr_; }
    friend bool operator==(const ComPtr& a, T* b) noexcept { return a.ptr_ == b; }
    friend bool operator!=(const ComPtr& a, T* b) noexcept { return a.ptr_ != b; }
    friend bool operator==(T* a, const ComPtr& b) noexcept { return a == b.ptr_; }
    friend bool operator!=(T* a, const ComPtr& b) noexcept { return a != b.ptr_; }
private:
    void InternalAddRef() noexcept { if (ptr_) ptr_->AddRef(); }
    void InternalAddRefOther(T* p) noexcept { if (p) p->AddRef(); }
    void InternalRelease() noexcept { T* p = ptr_; if (p) { ptr_ = nullptr; p->Release(); } }
    T* ptr_;
};
}} // namespace Microsoft::WRL
#ifndef IID_PPV_ARGS
template <typename T> inline void** Shim_IID_PPV_ARGS_Helper(T** pp) {
    return reinterpret_cast<void**>(pp);
}
#define IID_PPV_ARGS(ppType) __uuidof(**(ppType)), Shim_IID_PPV_ARGS_Helper(ppType)
#endif
