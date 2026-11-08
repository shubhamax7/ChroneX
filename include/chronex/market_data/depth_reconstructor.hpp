#pragma once

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "chronex/domain/types.hpp"

namespace chronex {

struct DepthLevel {
  Side side{};
  Price price{};
  Quantity quantity{};
  constexpr bool operator==(const DepthLevel&) const = default;
};

struct DepthSnapshot {
  std::uint64_t sequence{};
  std::vector<DepthLevel> levels;
};

struct DepthUpdate {
  std::uint64_t sequence{};
  Side side{};
  Price price{};
  Quantity quantity{};  // Absolute new quantity; zero removes the level.
};

struct ReconstructedDepth {
  std::uint64_t sequence{};
  std::optional<Price> best_bid;
  std::optional<Price> best_ask;
  std::vector<DepthLevel> levels;
};

class DepthReconstructor {
 public:
  explicit DepthReconstructor(Price price_max = 1'000'000, Quantity quantity_max = 1'000'000);
  void apply_snapshot(const DepthSnapshot& snapshot);
  void apply_update(const DepthUpdate& update);
  [[nodiscard]] ReconstructedDepth view() const;

 private:
  [[nodiscard]] bool valid_price(Price price) const noexcept;
  Price price_max_;
  Quantity quantity_max_;
  bool initialized_{};
  std::uint64_t sequence_{};
  std::map<Price, Quantity, std::greater<Price>> bids_;
  std::map<Price, Quantity> asks_;
};

}  // namespace chronex
