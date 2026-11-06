#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "progress.h"

namespace pd {

class ProgressRenderer : public ProgressSink {
 public:
  enum class Mode { kBar, kMachine, kQuiet };

  ProgressRenderer(Mode mode,
                   const std::vector<std::pair<std::string, uint64_t>>& parts);
  ~ProgressRenderer() override;

  void on_progress(const ProgressEvent& event) override;

  void start();
  // Stops the render thread and draws the final state.
  void stop();

 private:
  struct Entry {
    std::string name;
    uint64_t size = 0;
    int total_ops = 0;
    int completed_ops = 0;
    bool fec_phase = false;
    int phase_done = 0;
    int phase_total = 0;
    bool done = false;
    bool failed = false;
  };

  void render_locked(bool final_draw);
  int percent_for(const Entry& e) const;
  void maybe_print_machine(const ProgressEvent& event);
  void on_plain_event(const ProgressEvent& event);

  Mode mode_;
  bool tty_ = false;
  bool plain_ = false;
  std::vector<Entry> entries_;
  std::unordered_map<std::string, size_t> index_;
  std::unordered_map<std::string, int> last_percent_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool stop_ = false;
  bool dirty_ = true;
  bool started_ = false;
  int lines_drawn_ = 0;
};

}  // namespace pd
