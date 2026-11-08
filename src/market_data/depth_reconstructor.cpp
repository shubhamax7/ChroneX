#include "chronex/market_data/depth_reconstructor.hpp"

#include <limits>
#include <stdexcept>

namespace chronex {

DepthReconstructor::DepthReconstructor(const Price price_max, const Quantity quantity_max)
    : price_max_(price_max), quantity_max_(quantity_max) {
  if (price_max_ < 1 || quantity_max_ == 0U)
    throw std::invalid_argument("depth limits must be positive");
}

bool DepthReconstructor::valid_price(const Price price) const noexcept {
  return price >= 1 && price <= price_max_;
}

void DepthReconstructor::apply_snapshot(const DepthSnapshot& snapshot) {
  if (snapshot.sequence == 0U) throw std::invalid_argument("snapshot sequence must be positive");
  if (initialized_ && snapshot.sequence <= sequence_)
    throw std::invalid_argument("snapshot sequence must advance the current book");
  std::map<Price, Quantity, std::greater<Price>> next_bids;
  std::map<Price, Quantity> next_asks;
  for (const auto& level : snapshot.levels) {
    if (level.side != Side::Buy && level.side != Side::Sell)
      throw std::invalid_argument("snapshot side is invalid");
    if (!valid_price(level.price)) throw std::invalid_argument("snapshot price out of range");
    if (level.quantity == 0U || level.quantity > quantity_max_)
      throw std::invalid_argument("snapshot quantity out of range");
    const bool inserted = level.side == Side::Buy
                              ? next_bids.emplace(level.price, level.quantity).second
                              : next_asks.emplace(level.price, level.quantity).second;
    if (!inserted) throw std::invalid_argument("snapshot contains duplicate side and price");
  }
  if (!next_bids.empty() && !next_asks.empty() &&
      next_bids.begin()->first >= next_asks.begin()->first)
    throw std::invalid_argument("snapshot is locked or crossed");
  bids_.swap(next_bids);
  asks_.swap(next_asks);
  sequence_ = snapshot.sequence;
  initialized_ = true;
}

void DepthReconstructor::apply_update(const DepthUpdate& update) {
  if (!initialized_) throw std::logic_error("a depth snapshot is required before updates");
  if (sequence_ == std::numeric_limits<std::uint64_t>::max() || update.sequence != sequence_ + 1U)
    throw std::invalid_argument("depth update sequence gap");
  if (!valid_price(update.price)) throw std::invalid_argument("depth update price out of range");
  if (update.quantity > quantity_max_) throw std::invalid_argument("depth quantity out of range");
  if (update.side != Side::Buy && update.side != Side::Sell)
    throw std::invalid_argument("depth update side is invalid");

  const auto opposite_best = update.side == Side::Buy
                                 ? (asks_.empty() ? std::optional<Price>{} : asks_.begin()->first)
                                 : (bids_.empty() ? std::optional<Price>{} : bids_.begin()->first);
  if (update.quantity > 0U && opposite_best &&
      (update.side == Side::Buy ? update.price >= *opposite_best : update.price <= *opposite_best))
    throw std::invalid_argument("depth update would lock or cross the book");

  if (update.side == Side::Buy) {
    if (update.quantity == 0U) {
      if (!bids_.contains(update.price))
        throw std::invalid_argument("cannot remove absent bid level");
      bids_.erase(update.price);
    } else {
      bids_[update.price] = update.quantity;
    }
  } else if (update.quantity == 0U) {
    if (!asks_.contains(update.price))
      throw std::invalid_argument("cannot remove absent ask level");
    asks_.erase(update.price);
  } else {
    asks_[update.price] = update.quantity;
  }
  sequence_ = update.sequence;
}

ReconstructedDepth DepthReconstructor::view() const {
  ReconstructedDepth result;
  result.sequence = sequence_;
  if (!bids_.empty()) result.best_bid = bids_.begin()->first;
  if (!asks_.empty()) result.best_ask = asks_.begin()->first;
  result.levels.reserve(bids_.size() + asks_.size());
  for (const auto& [price, quantity] : bids_) result.levels.push_back({Side::Buy, price, quantity});
  for (const auto& [price, quantity] : asks_)
    result.levels.push_back({Side::Sell, price, quantity});
  return result;
}

}  // namespace chronex
