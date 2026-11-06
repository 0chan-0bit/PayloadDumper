#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace pd {

// Both worker groups share partition cursors and claim one operation at a
// time. They can converge on a busy partition without duplicating work.
class OperationScheduler {
 public:
  struct PartitionWork {
    uint64_t bytes;
    size_t operations;
    std::vector<uint64_t> costs{};
  };
  explicit OperationScheduler(const std::vector<PartitionWork>& parts) {
    for (size_t i = 0; i < parts.size(); ++i) {
      if (parts[i].operations) {
        std::vector<size_t> order;
        for (size_t op = 0; op < parts[i].operations; ++op) order.push_back(op);
        if (parts[i].costs.size() == order.size()) {
          std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return parts[i].costs[a] > parts[i].costs[b];
          });
        }
        queue_.push_back({i, parts[i].bytes, parts[i].operations, 0, std::move(order)});
      }
    }
    std::stable_sort(queue_.begin(), queue_.end(),
                     [](const Entry& a, const Entry& b) {
                       return a.bytes < b.bytes;
                     });
    end_ = queue_.size();
  }

  std::optional<std::pair<size_t, size_t>> take(bool largest_first) {
    std::lock_guard<std::mutex> lock(mu_);
    if (begin_ == end_) return std::nullopt;
    Entry& part = queue_[largest_first ? end_ - 1 : begin_];
    const auto result = std::make_pair(part.partition, part.order[part.next++]);
    if (part.next == part.operations) {
      if (largest_first) --end_;
      else ++begin_;
    }
    return result;
  }

 private:
  struct Entry {
    size_t partition;
    uint64_t bytes;
    size_t operations;
    size_t next;
    std::vector<size_t> order;
  };
  std::mutex mu_;
  std::vector<Entry> queue_;
  size_t begin_ = 0;
  size_t end_ = 0;
};

}  // namespace pd
