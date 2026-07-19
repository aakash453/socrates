#include <gtest/gtest.h>
#include "socrates/result.h"

namespace socrates {
namespace {

TEST(Result, Ok_HoldsValue) {
  auto r = Result<int>::Ok(42);
  EXPECT_TRUE(r.is_ok());
  EXPECT_FALSE(r.is_err());
  EXPECT_EQ(r.value(), 42);
}

TEST(Result, Err_HoldsError) {
  auto r = Result<int>::Err(ErrorCode::kNotFound, "missing");
  EXPECT_TRUE(r.is_err());
  EXPECT_FALSE(r.is_ok());
  EXPECT_EQ(r.error_code(), ErrorCode::kNotFound);
}

TEST(Result, OkNeverHasError) {
  auto r = Result<std::string>::Ok("hello");
  EXPECT_EQ(r.error_code(), ErrorCode::kOk);
}

TEST(Result, ImplicitValueConstruct) {
  Result<int> r = 42;
  EXPECT_TRUE(r.is_ok());
  EXPECT_EQ(r.value(), 42);
}

TEST(Result, ImplicitErrorConstruct) {
  Result<int> r = RuntimeError(ErrorCode::kInternal, "boom");
  EXPECT_TRUE(r.is_err());
  EXPECT_EQ(r.error_code(), ErrorCode::kInternal);
}

TEST(Result, MoveValue) {
  auto r = Result<std::string>::Ok("movable");
  std::string moved = r.take_value();
  EXPECT_EQ(moved, "movable");
}

TEST(Result, ErrorCodeFactory) {
  auto r = Result<double>::Err(ErrorCode::kResourceExhausted, "oom");
  EXPECT_EQ(r.error_code(), ErrorCode::kResourceExhausted);
}

}  // namespace
}  // namespace socrates
