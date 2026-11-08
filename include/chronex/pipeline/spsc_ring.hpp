#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace chronex {

// A bounded queue for exactly one producer and one consumer. A failed push is
// backpressure: the producer must retry or reject the command explicitly.
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity > 0U);
  static_assert(std::is_default_constructible_v<T>);
  static_assert(std::is_nothrow_move_constructible_v<T>);
  static_assert(std::is_nothrow_move_assignable_v<T>);

 public:
  [[nodiscard]] bool try_push(T&& value) noexcept {
    const auto write = write_index_.load(std::memory_order_relaxed);
    const auto read = read_index_.load(std::memory_order_acquire);
    if (write - read >= Capacity) return false;
    slots_[static_cast<std::size_t>(write % Capacity)] = std::move(value);
    write_index_.store(write + 1U, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::optional<T> try_pop() noexcept {
    const auto read = read_index_.load(std::memory_order_relaxed);
    const auto write = write_index_.load(std::memory_order_acquire);
    if (read == write) return std::nullopt;
    T value = std::move(slots_[static_cast<std::size_t>(read % Capacity)]);
    read_index_.store(read + 1U, std::memory_order_release);
    return value;
  }

  [[nodiscard]] std::size_t size_approx() const noexcept {
    const auto write = write_index_.load(std::memory_order_acquire);
    const auto read = read_index_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
  }

 private:
  alignas(64) std::atomic<std::uint64_t> write_index_{0};
  alignas(64) std::atomic<std::uint64_t> read_index_{0};
  std::array<T, Capacity> slots_{};
};

}  // namespace chronex
