#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace socrates {

enum class ErrorCode : std::int32_t {
  kOk = 0, kCancelled = 1, kInvalidArgument = 2, kNotFound = 3,
  kAlreadyExists = 4, kPermissionDenied = 5, kResourceExhausted = 6,
  kFailedPrecondition = 7, kUnavailable = 8, kDeadlineExceeded = 9,
  kDataLoss = 10, kUnauthenticated = 11, kInternal = 12,
};

struct RuntimeError : std::runtime_error {
  ErrorCode code{ErrorCode::kInternal};
  explicit RuntimeError(ErrorCode c = ErrorCode::kInternal, std::string msg = "")
      : std::runtime_error(std::move(msg)), code(c) {}
  [[nodiscard]] bool ok() const { return code == ErrorCode::kOk; }
  [[nodiscard]] std::string_view message() const { return what(); }
};

template <typename T>
class Result {
 public:
  Result(T v) : ok_(true), value_(std::move(v)) {}               // NOLINT
  Result(RuntimeError e) : ok_(false), error_(std::move(e)) {}   // NOLINT
  Result(const Result&) = default;
  Result(Result&&) noexcept = default;
  Result& operator=(const Result&) = default;
  Result& operator=(Result&&) noexcept = default;

  static Result Ok(T v) { return Result(std::move(v)); }
  static Result Err(RuntimeError e) { return Result(std::move(e)); }
  static Result Err(ErrorCode c, std::string m) { return Result(RuntimeError{c, std::move(m)}); }

  [[nodiscard]] bool is_ok() const { return ok_; }
  [[nodiscard]] bool is_err() const { return !ok_; }
  [[nodiscard]] const T& value() const { return *value_; }
  [[nodiscard]] T& value() { return *value_; }
  [[nodiscard]] T&& take_value() { return std::move(*value_); }
  [[nodiscard]] T value_or(T fallback) const {
    return ok_ ? *value_ : std::move(fallback);
  }
  [[nodiscard]] const RuntimeError& error() const { return *error_; }
  [[nodiscard]] ErrorCode error_code() const { return ok_ ? ErrorCode::kOk : error_->code; }

 private:
  bool ok_{true};
  std::optional<T> value_;
  std::optional<RuntimeError> error_;
};

}  // namespace socrates
