#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace pd {

struct ProgressEvent {
  const std::string* partition = nullptr;
  int total_ops = 0;
  int completed_ops = 0;
  bool fec_phase = false;
  int phase_completed = 0;
  int phase_total = 0;
  bool done = false;
  bool failed = false;
};

class ProgressSink {
 public:
  virtual ~ProgressSink() = default;
  // May be called concurrently from several threads.
  virtual void on_progress(const ProgressEvent& event) = 0;
};

}  // namespace pd
