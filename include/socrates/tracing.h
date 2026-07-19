// include/socrates/tracing.h
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "socrates/types.h"

namespace socrates {

class TraceRecorder;

class TraceSpan {
 public:
  TraceSpan(std::string_view name, TraceRecorder& recorder);
  ~TraceSpan();
  TraceSpan(const TraceSpan&) = delete;
  TraceSpan& operator=(const TraceSpan&) = delete;

  void set_attribute(std::string_view key, std::string_view value);
  void set_attribute(std::string_view key, std::int64_t value);
  void set_attribute(std::string_view key, double value);
  void set_attribute(std::string_view key, bool value);
  void add_event(std::string_view name);

 private:
  std::string span_id_;
  TraceRecorder& recorder_;
  Clock::time_point start_time_;
};

class TraceRecorder {
 public:
  explicit TraceRecorder(TraceId trace_id);
  ~TraceRecorder();
  TraceRecorder(const TraceRecorder&) = delete;
  TraceRecorder& operator=(const TraceRecorder&) = delete;

  void set_root_attribute(std::string_view key, std::string_view value);
  std::unique_ptr<TraceSpan> start_span(std::string_view name);

  // Internal — used by TraceSpan
  void record_span_start(std::string_view span_id, std::string_view name,
                         Clock::time_point start);
  void record_span_end(std::string_view span_id, Clock::time_point end);
  void record_span_attribute(std::string_view span_id, std::string_view key,
                             std::string_view value);
  void record_span_event(std::string_view span_id, std::string_view event_name);

 private:
  TraceId trace_id_;
};

}  // namespace socrates
