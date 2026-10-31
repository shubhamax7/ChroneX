#pragma once

#include <variant>

#include "chronex/domain/types.hpp"

namespace chronex {

struct LimitGtc {
  OrderId order_id{};
  Side side{};
  Price price{};
  Quantity quantity{};
};

struct LimitIoc {
  OrderId order_id{};
  Side side{};
  Price price{};
  Quantity quantity{};
};

struct MarketOrder {
  OrderId order_id{};
  Side side{};
  Quantity quantity{};
  Price protection_ticks{};
};

struct CancelOrder {
  OrderId order_id{};
};

struct ReplaceOrder {
  OrderId order_id{};
  Price new_price{};
  Quantity new_quantity{};
};

using CommandBody = std::variant<LimitGtc, LimitIoc, MarketOrder, CancelOrder, ReplaceOrder>;

struct Command {
  CommandSequence sequence{};
  CommandBody body{};
};

}  // namespace chronex
