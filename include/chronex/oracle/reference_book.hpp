#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chronex/domain/command.hpp"
#include "chronex/domain/event.hpp"

namespace chronex {

[[nodiscard]] std::uint64_t allocation_count_snapshot() noexcept;

struct BookConfig {
  Price price_max{1'000'000};
  Quantity quantity_max{1'000'000};
};

struct RestingOrder {
  OrderId order_id{};
  Side side{};
  Price price{};
  Quantity remaining_quantity{};
  AcceptanceSequence acceptance_sequence{};

  constexpr bool operator==(const RestingOrder&) const = default;
};

class ReferenceBook {
 public:
  explicit ReferenceBook(BookConfig config = {});
  ~ReferenceBook();

  ReferenceBook(const ReferenceBook&) = delete;
  ReferenceBook& operator=(const ReferenceBook&) = delete;
  ReferenceBook(ReferenceBook&&) noexcept;
  ReferenceBook& operator=(ReferenceBook&&) noexcept;

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
