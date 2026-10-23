#pragma once
#include <memory>
#include <type_traits>
#include <utility>
namespace android {
template <typename T>
class sp {
  public:
    sp() = default;
    sp(std::nullptr_t) {}
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    sp(const sp<U>& other) : p_(other.p_) {}
    template <typename... Args>
    static sp make(Args&&... args) {
        sp s;
        s.p_ = std::make_shared<T>(std::forward<Args>(args)...);
        return s;
    }
    T* operator->() const { return p_.get(); }
    T& operator*() const { return *p_; }
    explicit operator bool() const { return static_cast<bool>(p_); }
    T* get() const { return p_.get(); }
    bool operator==(std::nullptr_t) const { return !p_; }
    bool operator!=(std::nullptr_t) const { return static_cast<bool>(p_); }

  private:
    template <typename>
    friend class sp;
    std::shared_ptr<T> p_;
};
}  // namespace android
