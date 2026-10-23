#pragma once
#include <error/expected_utils.h>
#include <optional>
#include <string>
#include <utility>

namespace android {
template <typename T>
class Result {
  public:
    Result(T value) : value_(std::move(value)) {}
    explicit Result(std::string error) : error_(std::move(error)) {}
    bool ok() const { return value_.has_value(); }
    const T& value() const { return *value_; }
    T&& value() { return std::move(*value_); }
    const std::string& error() const { return error_; }

  private:
    std::optional<T> value_;
    std::string error_;
};
}  // namespace android
