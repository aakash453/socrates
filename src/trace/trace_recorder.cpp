// src/trace/trace_recorder.cpp
// Trace lifecycle, span tree, and atomic export without sensitive payloads.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "socrates/tracing.h"

namespace socrates {

struct SpanRecord {
  std::string span_id;
  std::string parent_span_id;
  std::string name;
  Clock::time_point start_time;
  Clock::time_point end_time;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<std::pair<std::string, Clock::time_point>> events;
  bool ended{false};
};

class TraceRecorderImpl {
 public:
  explicit TraceRecorderImpl(TraceId trace_id) : trace_id_(std::move(trace_id)) {
    root_start_ = Clock::now();
  }

  void set_root_attribute(const std::string& key, const std::string& value) {
    std::lock_guard lock(mutex_);
    root_attributes_[key] = value;
  }

  void on_span_start(const std::string& span_id, const std::string& name,
                      Clock::time_point start) {
    std::lock_guard lock(mutex_);
    SpanRecord rec;
    rec.span_id = span_id;
    rec.name = name;
    rec.start_time = start;
    spans_[span_id] = std::move(rec);
  }

  void on_span_end(const std::string& span_id, Clock::time_point end) {
    std::lock_guard lock(mutex_);
    auto it = spans_.find(span_id);
    if (it != spans_.end()) {
      it->second.end_time = end;
      it->second.ended = true;
    }
  }

  void on_span_set_attribute(const std::string& span_id, const std::string& key,
                              const std::string& value) {
    std::lock_guard lock(mutex_);
    auto it = spans_.find(span_id);
    if (it != spans_.end()) {
      it->second.attributes.emplace_back(key, value);
    }
  }

  void on_span_add_event(const std::string& span_id,
                          const std::string& event_name) {
    std::lock_guard lock(mutex_);
    auto it = spans_.find(span_id);
    if (it != spans_.end()) {
      it->second.events.emplace_back(event_name, Clock::now());
    }
  }

  std::string export_json() const {
    std::lock_guard lock(mutex_);
    std::ostringstream os;
    os << "{\n  \"trace_id\": \"" << trace_id_.value << "\",\n";
    os << "  \"start_time_ns\": " << root_start_.time_since_epoch().count() << ",\n";
    os << "  \"duration_ns\": "
       << (Clock::now() - root_start_).count() << ",\n";
    os << "  \"attributes\": {";
    bool first = true;
    for (const auto& [k, v] : root_attributes_) {
      if (!first) os << ",";
      os << "\n    \"" << k << "\": \"" << v << "\"";
      first = false;
    }
    os << "\n  },\n  \"spans\": [";
    bool first_span = true;
    for (const auto& [id, span] : spans_) {
      if (!first_span) os << ",";
      os << "\n    {";
      os << "\n      \"span_id\": \"" << span.span_id << "\",";
      os << "\n      \"name\": \"" << span.name << "\",";
      os << "\n      \"start_ns\": " << span.start_time.time_since_epoch().count() << ",";
      os << "\n      \"end_ns\": " << span.end_time.time_since_epoch().count() << ",";
      os << "\n      \"ended\": " << (span.ended ? "true" : "false") << ",";
      os << "\n      \"attributes\": {";
      bool first_attr = true;
      for (const auto& [k, v] : span.attributes) {
        if (!first_attr) os << ",";
        os << "\n        \"" << k << "\": \"" << v << "\"";
        first_attr = false;
      }
      os << "\n      },";
      os << "\n      \"events\": [";
      bool first_ev = true;
      for (const auto& [name, ts] : span.events) {
        if (!first_ev) os << ",";
        os << "\n        {\"name\": \"" << name
           << "\", \"ts_ns\": " << ts.time_since_epoch().count() << "}";
        first_ev = false;
      }
      os << "\n      ]";
      os << "\n    }";
      first_span = false;
    }
    os << "\n  ]\n}\n";
    return os.str();
  }

  TraceId trace_id_;
  Clock::time_point root_start_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::string> root_attributes_;
  std::unordered_map<std::string, SpanRecord> spans_;
};

TraceRecorder::TraceRecorder(TraceId trace_id)
    : trace_id_(std::move(trace_id)) {}

TraceRecorder::~TraceRecorder() = default;

void TraceRecorder::set_root_attribute(std::string_view key, std::string_view value) {
  if (!impl_) impl_ = std::make_unique<TraceRecorderImpl>(trace_id_);
  impl_->set_root_attribute(std::string(key), std::string(value));
}

std::unique_ptr<TraceSpan> TraceRecorder::start_span(std::string_view name) {
  if (!impl_) impl_ = std::make_unique<TraceRecorderImpl>(trace_id_);
  return std::unique_ptr<TraceSpan>(new TraceSpan(name, *this));
}

void TraceRecorder::on_span_start(std::string_view span_id, std::string_view name,
                                   Clock::time_point start) {
  if (impl_) impl_->on_span_start(std::string(span_id), std::string(name), start);
}

void TraceRecorder::on_span_end(std::string_view span_id, Clock::time_point end) {
  if (impl_) impl_->on_span_end(std::string(span_id), end);
}

void TraceRecorder::on_span_set_attribute(std::string_view span_id,
                                           std::string_view key,
                                           std::string_view value) {
  if (impl_) impl_->on_span_set_attribute(std::string(span_id), std::string(key),
                                           std::string(value));
}

void TraceRecorder::on_span_add_event(std::string_view span_id,
                                       std::string_view event_name) {
  if (impl_) impl_->on_span_add_event(std::string(span_id), std::string(event_name));
}

}  // namespace socrates
