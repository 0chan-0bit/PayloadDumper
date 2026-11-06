#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace pd {

// Fixed-size worker pool. Tasks submitted from outside the pool may use
// parallel_for(); parallel_for is never called from inside a pool task.
class ThreadPool {
 public:
  explicit ThreadPool(size_t threads) {
    if (threads == 0) threads = 1;
    threads_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
      threads_.emplace_back([this] { worker_loop(); });
    }
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) {
      if (t.joinable()) t.join();
    }
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  size_t size() const { return threads_.size(); }

  void submit(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mu_);
    idle_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
  }

  // Runs fn(0..n-1) on the pool only. Must not be called
  // from a pool worker (callers are the extract thread).
  template <typename F>
  void parallel_for(size_t n, F&& fn) {
    if (n == 0) return;
    std::atomic<size_t> next{0};
    std::mutex completion_mu;
    std::condition_variable completion_cv;
    std::exception_ptr error;
    size_t helpers_done = 0;
    const size_t helpers = n < threads_.size() ? n : threads_.size();
    auto helper = [&] {
      try {
        for (;;) {
          size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= n) break;
          fn(i);
        }
      } catch (...) {
        std::lock_guard<std::mutex> lock(completion_mu);
        if (!error) error = std::current_exception();
      }
      std::lock_guard<std::mutex> lock(completion_mu);
      ++helpers_done;
      completion_cv.notify_one();
    };
    for (size_t i = 0; i < helpers; ++i) submit(helper);
    std::unique_lock<std::mutex> lock(completion_mu);
    completion_cv.wait(lock, [&] { return helpers_done == helpers; });
    if (error) std::rethrow_exception(error);
  }

 private:
  void worker_loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) return;
        task = std::move(queue_.front());
        queue_.pop_front();
        ++active_;
      }
      task();
      {
        std::lock_guard<std::mutex> lock(mu_);
        --active_;
        if (queue_.empty() && active_ == 0) idle_.notify_all();
      }
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable idle_;
  std::deque<std::function<void()>> queue_;
  size_t active_ = 0;
  bool stop_ = false;
  std::vector<std::thread> threads_;
};

}  // namespace pd
