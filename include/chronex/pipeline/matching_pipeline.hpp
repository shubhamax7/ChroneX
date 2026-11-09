#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "chronex/oracle/reference_book.hpp"
#include "chronex/pipeline/spsc_ring.hpp"

namespace chronex {

struct PipelineResult {
  CommandSequence sequence{};
  std::vector<Event> events;

  PipelineResult() = default;
  PipelineResult(CommandSequence command_sequence, std::vector<Event> command_events) noexcept
      : sequence(command_sequence), events(std::move(command_events)) {}
  PipelineResult(PipelineResult&&) noexcept = default;
  PipelineResult& operator=(PipelineResult&&) noexcept = default;
  PipelineResult(const PipelineResult&) = delete;
  PipelineResult& operator=(const PipelineResult&) = delete;
};

// The caller is the sole submitter and result consumer. Queue-full submit
// returns false without accepting the command. drain_and_stop returns every
// accepted result, including those still in flight when shutdown starts.
template <std::size_t QueueCapacity>
class MatchingPipeline {
 public:
  explicit MatchingPipeline(BookConfig config = {}, const std::vector<Command>& recovery_log = {})
      : book_(config) {
    for (const auto& command : recovery_log) {
      static_cast<void>(book_.process(command));
      if (command.sequence == std::numeric_limits<CommandSequence>::max()) {
        sequence_exhausted_ = true;
      } else {
        next_sequence_ = command.sequence + 1U;
      }
    }
    worker_running_.store(true, std::memory_order_relaxed);
    worker_ = std::thread([this] { run(); });
  }
  ~MatchingPipeline() {
    try {
      static_cast<void>(drain_and_stop());
    } catch (...) {
    }
  }
  MatchingPipeline(const MatchingPipeline&) = delete;
  MatchingPipeline& operator=(const MatchingPipeline&) = delete;

  [[nodiscard]] bool try_submit(Command command) {
    if (stopping_.load(std::memory_order_acquire)) return false;
    if (sequence_exhausted_ || command.sequence != next_sequence_)
      throw std::invalid_argument("pipeline commands must have contiguous sequence numbers");
    if (!commands_.try_push(std::move(command))) return false;
    if (next_sequence_ == std::numeric_limits<CommandSequence>::max())
      sequence_exhausted_ = true;
    else
      ++next_sequence_;
    return true;
  }

  [[nodiscard]] std::optional<PipelineResult> try_receive() noexcept {
    return results_.try_pop();
  }

  [[nodiscard]] bool has_capacity() const noexcept {
    return !stopping_.load(std::memory_order_acquire) && commands_.size_approx() < QueueCapacity;
  }

  [[nodiscard]] std::vector<PipelineResult> drain_and_stop() {
    if (!joined_) {
      stopping_.store(true, std::memory_order_release);
      std::vector<PipelineResult> remaining;
      while (worker_running_.load(std::memory_order_acquire)) {
        if (auto result = results_.try_pop()) {
          remaining.push_back(std::move(*result));
        } else {
          std::this_thread::yield();
        }
      }
      if (worker_.joinable()) worker_.join();
      while (auto result = results_.try_pop()) remaining.push_back(std::move(*result));
      joined_ = true;
      if (worker_error_) std::rethrow_exception(worker_error_);
      return remaining;
    }
    return {};
  }

 private:
  void run() noexcept {
    try {
      for (;;) {
        if (auto command = commands_.try_pop()) {
          const auto sequence = command->sequence;
          PipelineResult result(sequence, book_.process(*command));
          while (!results_.try_push(std::move(result))) std::this_thread::yield();
          continue;
        }
        if (stopping_.load(std::memory_order_acquire)) break;
        std::this_thread::yield();
      }
    } catch (...) {
      worker_error_ = std::current_exception();
    }
    worker_running_.store(false, std::memory_order_release);
  }

  SpscRing<Command, QueueCapacity> commands_;
  SpscRing<PipelineResult, QueueCapacity> results_;
  ReferenceBook book_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> worker_running_{false};
  std::thread worker_;
  std::exception_ptr worker_error_;
  CommandSequence next_sequence_{1};
  bool sequence_exhausted_{};
  bool joined_{};
};

}  // namespace chronex
