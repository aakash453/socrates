// include/socrates/cancellation.h
// Portable stop-token polyfill for platforms where CancellationToken is unavailable.
#pragma once

#include <atomic>
#include <memory>

namespace socrates {

class CancellationToken {
 public:
  CancellationToken() : stopped_(std::make_shared<std::atomic<bool>>(false)) {}
  CancellationToken(const CancellationToken&) = default;
  CancellationToken& operator=(const CancellationToken&) = default;

  [[nodiscard]] bool stop_requested() const { return stopped_->load(); }
  void request_stop() { stopped_->store(true); }

 private:
  std::shared_ptr<std::atomic<bool>> stopped_;
};

}  // namespace socrates
