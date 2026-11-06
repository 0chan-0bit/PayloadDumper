#include "operation_scheduler.h"
#include "pool.h"

#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

void check(bool ok) {
  if (!ok) throw std::runtime_error("operation scheduler check failed");
}

int main() {
  // parallel_for must never use the calling thread or exceed pool size, and
  // exceptions must reach the caller after all helper lifetimes are safe.
  for (size_t workers : {size_t{1}, size_t{4}}) {
    pd::ThreadPool pool(workers);
    const auto caller = std::this_thread::get_id();
    std::atomic<int> active{0}, peak{0}, visits{0};
    std::atomic<bool> used_caller{false};
    pool.parallel_for(100, [&](size_t) {
      if (std::this_thread::get_id() == caller) used_caller.store(true);
      int current = active.fetch_add(1) + 1;
      int previous = peak.load();
      while (previous < current && !peak.compare_exchange_weak(previous, current)) {}
      visits.fetch_add(1);
      std::this_thread::yield();
      active.fetch_sub(1);
    });
    check(!used_caller.load() && peak.load() <= static_cast<int>(workers));
    check(visits.load() == 100);
    bool caught = false;
    try {
      pool.parallel_for(10, [](size_t) { throw std::runtime_error("expected"); });
    } catch (const std::runtime_error&) { caught = true; }
    check(caught);
    pool.parallel_for(0, [](size_t) { check(false); });
    pool.parallel_for(1, [&](size_t) { visits.fetch_add(1); });
    check(visits.load() == 101);
  }
  using Scheduler = pd::OperationScheduler;
  Scheduler order({{20, 2}, {10, 1}, {30, 2}, {0, 0}});
  check(order.take(true) == std::make_pair(size_t{2}, size_t{0}));
  check(order.take(false) == std::make_pair(size_t{1}, size_t{0}));
  check(order.take(true) == std::make_pair(size_t{2}, size_t{1}));
  check(order.take(false) == std::make_pair(size_t{0}, size_t{0}));
  check(order.take(true) == std::make_pair(size_t{0}, size_t{1}));
  check(!order.take(true) && !order.take(false));
  Scheduler empty({});
  check(!empty.take(true) && !empty.take(false));
  Scheduler ties({{10, 1}, {10, 1}});
  check(ties.take(false)->first == 0);
  check(ties.take(true)->first == 1);
  Scheduler costs({{100, 4, {1, 20, 20, 5}}});
  check(costs.take(false)->second == 1);
  check(costs.take(true)->second == 2);
  check(costs.take(false)->second == 3);
  check(costs.take(true)->second == 0);

  // A busy worker blocks inside operation 0. The other worker drains the
  // small partition and takes the remaining operations of the large one.
  Scheduler tail({{100, 3}, {1, 1}});
  std::promise<void> started;
  auto ready = started.get_future();
  std::promise<void> release;
  auto resume = release.get_future();
  std::optional<std::pair<size_t, size_t>> busy_task;
  std::thread busy([&] {
    busy_task = tail.take(true);
    started.set_value();
    resume.wait();
  });
  ready.wait();
  const auto small = tail.take(false);
  const auto stolen1 = tail.take(false);
  const auto stolen2 = tail.take(false);
  const bool drained = !tail.take(false);
  release.set_value();
  busy.join();
  check(busy_task == std::make_pair(size_t{0}, size_t{0}));
  check(small == std::make_pair(size_t{1}, size_t{0}));
  check(stolen1 == std::make_pair(size_t{0}, size_t{1}));
  check(stolen2 == std::make_pair(size_t{0}, size_t{2}));
  check(drained);

  for (int workers : {4, 5, 8, 12}) {
    for (int partitions : {1, 17}) {
      for (size_t operations : {size_t{3}, size_t{1001}}) {
        std::vector<Scheduler::PartitionWork> parts;
        for (int p = 0; p < partitions; ++p) {
          parts.push_back({static_cast<uint64_t>((p * 7) % 17), operations});
        }
        Scheduler scheduler(parts);
        std::vector<std::atomic<int>> seen(partitions * operations);
        for (auto& count : seen) count.store(0);
        std::vector<std::thread> threads;
        for (int i = 0; i < workers; ++i) {
          threads.emplace_back([&, large_first = i < workers / 4] {
            while (auto task = scheduler.take(large_first)) {
              seen.at(task->first * operations + task->second).fetch_add(1);
              std::this_thread::yield();
            }
          });
        }
        for (auto& thread : threads) thread.join();
        for (auto& count : seen) check(count.load() == 1);
      }
    }
  }
  std::puts("operation scheduler checks passed");
}
