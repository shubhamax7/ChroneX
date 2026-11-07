#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chronex/oracle/reference_book.hpp"

namespace chronex {

// Fixed-capacity price-indexed book with pooled order slots and intrusive FIFO
// links. Capacity is explicit so failure is deterministic and reported.
struct OptimizedBookConfig {
  BookConfig limits{};
  std::size_t order_capacity{65'536};
};

class OptimizedBook {
 public:
  explicit OptimizedBook(OptimizedBookConfig config = {});
  ~OptimizedBook();
  OptimizedBook(const OptimizedBook&) = delete;
  OptimizedBook& operator=(const OptimizedBook&) = delete;
  OptimizedBook(OptimizedBook&&) noexcept;
  OptimizedBook& operator=(OptimizedBook&&) noexcept;

  [[nodiscard]] std::vector<Event> process(const Command& command);
  [[nodiscard]] std::vector<RestingOrder> resting_orders(Side side) const;
  [[nodiscard]] std::optional<Price> best_price(Side side) const;
  [[nodiscard]] std::size_t live_order_count() const noexcept;
  [[nodiscard]] std::optional<std::string> invariant_violation() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace chronex
