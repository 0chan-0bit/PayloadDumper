#include "progress_renderer.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "util.h"

namespace pd {
namespace {

constexpr int kDefaultWidth = 100;
constexpr int kDefaultRows = 24;

void terminal_size(int* rows, int* cols) {
  *rows = kDefaultRows;
  *cols = kDefaultWidth;
  struct winsize ws {};
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_row > 2) *rows = ws.ws_row;
    if (ws.ws_col > 40) *cols = ws.ws_col;
  }
}

}  // namespace

ProgressRenderer::ProgressRenderer(
    Mode mode, const std::vector<std::pair<std::string, uint64_t>>& parts)
    : mode_(mode) {
  tty_ = isatty(STDERR_FILENO) != 0;
  if (mode_ == Mode::kBar && !tty_) plain_ = true;
  entries_.reserve(parts.size());
  for (const auto& p : parts) {
    Entry e;
    e.name = p.first;
    e.size = p.second;
    index_[e.name] = entries_.size();
    entries_.push_back(std::move(e));
  }
}

ProgressRenderer::~ProgressRenderer() { stop(); }

void ProgressRenderer::start() {
  if (mode_ == Mode::kQuiet || plain_ || mode_ == Mode::kMachine) return;
  started_ = true;
  fprintf(stderr, "\x1b[?25l");  // hide cursor
  std::lock_guard<std::mutex> lock(mu_);
  thread_ = std::thread([this] {
    for (;;) {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait_for(lock, std::chrono::milliseconds(100),
                   [this] { return stop_; });
      if (stop_) break;
      if (dirty_) {
        render_locked(false);
        dirty_ = false;
      }
    }
  });
}

void ProgressRenderer::stop() {
  if (mode_ == Mode::kMachine) return;
  if (started_) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    started_ = false;
    std::lock_guard<std::mutex> lock(mu_);
    render_locked(true);
    fprintf(stderr, "\x1b[?25h");  // show cursor
    return;
  }
}

int ProgressRenderer::percent_for(const Entry& e) const {
  if (e.done) return 100;
  if (e.fec_phase && e.phase_total > 0) {
    int total = e.total_ops + e.phase_total;
    int done = e.total_ops + e.phase_done;
    if (total <= 0) return 100;
    return std::min(100, done * 100 / total);
  }
  if (e.total_ops <= 0) return 0;
  return std::min(100, e.completed_ops * 100 / e.total_ops);
}

void ProgressRenderer::on_progress(const ProgressEvent& event) {
  if (mode_ == Mode::kQuiet) return;
  if (mode_ == Mode::kMachine) {
    maybe_print_machine(event);
    return;
  }
  if (plain_) {
    on_plain_event(event);
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = index_.find(*event.partition);
  if (it == index_.end()) return;
  Entry& e = entries_[it->second];
  e.total_ops = event.total_ops;
  e.completed_ops = event.completed_ops;
  e.fec_phase = event.fec_phase;
  e.phase_done = event.phase_completed;
  e.phase_total = event.phase_total;
  if (event.done) e.done = true;
  if (event.failed) e.failed = true;
  dirty_ = true;
  cv_.notify_all();
}

void ProgressRenderer::on_plain_event(const ProgressEvent& event) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = index_.find(*event.partition);
  if (it == index_.end()) return;
  Entry& e = entries_[it->second];
  e.total_ops = event.total_ops;
  e.completed_ops = event.completed_ops;
  e.done = event.done;
  if (event.done) {
    fprintf(stderr, "%s: done (%s)\n", e.name.c_str(),
            human_bytes(e.size).c_str());
  }
}

void ProgressRenderer::maybe_print_machine(const ProgressEvent& event) {
  int percent = 100;
  if (!event.done) {
    if (event.total_ops > 0) {
      percent = event.completed_ops * 100 / event.total_ops;
    } else {
      percent = 0;
    }
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = last_percent_.find(*event.partition);
  if (it != last_percent_.end() && percent <= it->second) return;
  last_percent_[*event.partition] = percent;
  printf("%s:%d\n", event.partition->c_str(), percent);
  fflush(stdout);
}

void ProgressRenderer::render_locked(bool final_draw) {
  int rows = kDefaultRows;
  int cols = kDefaultWidth;
  terminal_size(&rows, &cols);

  const size_t max_lines = static_cast<size_t>(std::max(2, rows - 1));
  size_t shown = entries_.size();
  if (shown > max_lines) shown = max_lines - 1;

  if (lines_drawn_ > 0) {
    fprintf(stderr, "\x1b[%dA", lines_drawn_);
  }
  int drawn = 0;
  for (size_t i = 0; i < shown; ++i) {
    const Entry& e = entries_[i];
    int percent = percent_for(e);
    int bar_width = cols - 36;
    if (bar_width < 10) bar_width = 10;
    int filled = bar_width * percent / 100;
    std::string name = e.name;
    if (name.size() > 18) name = name.substr(0, 18);
    std::string size = human_bytes(e.size);
    std::string bar;
    bar.reserve(static_cast<size_t>(bar_width));
    for (int k = 0; k < bar_width; ++k) {
      if (k < filled) {
        bar.push_back('=');
      } else if (k == filled) {
        bar.push_back('>');
      } else {
        bar.push_back(' ');
      }
    }
    if (e.fec_phase && !e.done && percent < 100) {
      fprintf(stderr, "\x1b[2K\r%-18s %8s [%s] %3d%% verity\n", name.c_str(),
              size.c_str(), bar.c_str(), percent);
    } else {
      fprintf(stderr, "\x1b[2K\r%-18s %8s [%s] %3d%%\n", name.c_str(),
              size.c_str(), bar.c_str(), percent);
    }
    ++drawn;
  }
  if (entries_.size() > shown) {
    fprintf(stderr, "\x1b[2K\r... %zu more partitions\n",
            entries_.size() - shown);
    ++drawn;
  }
  fflush(stderr);
  lines_drawn_ = drawn;
  if (final_draw) {
    // Move past the block so subsequent output starts on a fresh line.
    fprintf(stderr, "\n");
  }
}

}  // namespace pd
