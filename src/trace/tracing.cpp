#include "socrates/tracing.h"

#include <random>
#include <sstream>

namespace socrates {

namespace {
std::string make_span_id() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::ostringstream os;
  os << std::hex << rng();
  return os.str();
}
}  // namespace

TraceSpan::TraceSpan(std::string_view name, TraceRecorder& recorder)
    : span_id_(make_span_id()), recorder_(recorder), start_time_(Clock::now()) {
  recorder_.record_span_start(span_id_, name, start_time_);
}

TraceSpan::~TraceSpan() {
  recorder_.record_span_end(span_id_, Clock::now());
}

void TraceSpan::set_attribute(std::string_view key, std::string_view value) {
  recorder_.record_span_attribute(span_id_, key, value);
}
void TraceSpan::set_attribute(std::string_view key, std::int64_t value) {
  recorder_.record_span_attribute(span_id_, key, std::to_string(value));
}
void TraceSpan::set_attribute(std::string_view key, double value) {
  recorder_.record_span_attribute(span_id_, key, std::to_string(value));
}
void TraceSpan::set_attribute(std::string_view key, bool value) {
  recorder_.record_span_attribute(span_id_, key, value ? "true" : "false");
}
void TraceSpan::add_event(std::string_view name) {
  recorder_.record_span_event(span_id_, name);
}

TraceRecorder::TraceRecorder(TraceId trace_id) : trace_id_(std::move(trace_id)) {}
TraceRecorder::~TraceRecorder() = default;

void TraceRecorder::set_root_attribute(std::string_view key, std::string_view value) {
  (void)key; (void)value;  // Full storage deferred to task 6.8
}

std::unique_ptr<TraceSpan> TraceRecorder::start_span(std::string_view name) {
  return std::make_unique<TraceSpan>(name, *this);
}

void TraceRecorder::record_span_start(std::string_view span_id, std::string_view name,
                                       Clock::time_point start) {
  (void)span_id; (void)name; (void)start;
}
void TraceRecorder::record_span_end(std::string_view span_id, Clock::time_point end) {
  (void)span_id; (void)end;
}
void TraceRecorder::record_span_attribute(std::string_view span_id, std::string_view key,
                                           std::string_view value) {
  (void)span_id; (void)key; (void)value;
}
void TraceRecorder::record_span_event(std::string_view span_id, std::string_view event) {
  (void)span_id; (void)event;
}

}  // namespace socrates
