// include/socrates/logging.h
// Minimal C++20 logger — uses std::format, no spdlog/fmt dependency.
#pragma once

#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#include "socrates/types.h"

namespace socrates {

enum class Severity : int { kTrace, kDebug, kInfo, kWarn, kError, kFatal };

class LogContext {
 public:
  explicit LogContext(std::string subsystem) : subsystem_(std::move(subsystem)) {}

  LogContext& with_node_id(NodeId id) {
    node_id_ = std::move(id.value);
    return *this;
  }
  LogContext& with_request_id(RequestId id) {
    request_id_ = std::move(id.value);
    return *this;
  }
  LogContext& with_trace_id(TraceId id) {
    trace_id_ = std::move(id.value);
    return *this;
  }
  LogContext& with_term(std::uint64_t term) {
    term_ = term;
    return *this;
  }
  LogContext& with_plan_id(PlanId id) {
    plan_id_ = std::move(id.value);
    return *this;
  }

  void trace(std::string_view msg) const { log(Severity::kTrace, msg); }
  void debug(std::string_view msg) const { log(Severity::kDebug, msg); }
  void info(std::string_view msg) const { log(Severity::kInfo, msg); }
  void warn(std::string_view msg) const { log(Severity::kWarn, msg); }
  void error(std::string_view msg) const { log(Severity::kError, msg); }
  void fatal(std::string_view msg) const { log(Severity::kFatal, msg); }

  void log(Severity severity, std::string_view msg) const {
    static const char* levels[] = {"TRC", "DBG", "INF", "WRN", "ERR", "FTL"};
    static std::mutex mtx;

    std::string ctx;
    if (!node_id_.empty()) ctx += " node=" + node_id_;
    if (!request_id_.empty()) ctx += " req=" + request_id_;
    if (!trace_id_.empty()) ctx += " trace=" + trace_id_;
    if (term_ != 0) ctx += " term=" + std::to_string(term_);
    if (!plan_id_.empty()) ctx += " plan=" + plan_id_;

    auto level = levels[static_cast<int>(severity)];
    std::lock_guard lock(mtx);
    std::cerr << std::format("[{}] {} {}{}", level, subsystem_, msg, ctx) << std::endl;
  }

 private:
  std::string subsystem_;
  std::string node_id_;
  std::string request_id_;
  std::string trace_id_;
  std::string plan_id_;
  std::uint64_t term_{0};
};

inline LogContext make_log_context(std::string subsystem) {
  return LogContext(std::move(subsystem));
}

}  // namespace socrates
