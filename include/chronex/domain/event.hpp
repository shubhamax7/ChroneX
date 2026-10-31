#pragma once

#include <optional>

#include "chronex/domain/types.hpp"

namespace chronex {

enum class RejectReason : std::uint8_t {
  InvalidPrice,
  InvalidQuantity,
  PriceOutOfBand,
  DuplicateOrderId,
  UnknownOrder,
  CapacityExceeded,
};

enum class EventKind : std::uint8_t {
  Acknowledged,
  Rejected,
  Filled,
  Rested,
  Cancelled,
};

struct Event {
  EventKind kind{};
  CommandSequence command_sequence{};
  std::optional<EventIndex> event_index{};
  std::optional<RejectReason> reject_reason{};
  OrderId order_id{};
  OrderId maker_order_id{};
  OrderId taker_order_id{};
  Price price{};
  Quantity quantity{};

  constexpr bool operator==(const Event&) const = default;
};

}  // namespace chronex
