#include "chronex/oracle/reference_book.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace chronex {
std::atomic<std::uint64_t> allocation_count{0};

namespace {

[[nodiscard]] Event acknowledgement(const CommandSequence command_sequence) {
  return Event{.kind = EventKind::Acknowledged, .command_sequence = command_sequence};
}

[[nodiscard]] Event rejection(const CommandSequence command_sequence, const RejectReason reason) {
  return Event{
      .kind = EventKind::Rejected, .command_sequence = command_sequence, .reject_reason = reason};
}

}  // namespace

std::uint64_t allocation_count_snapshot() noexcept {
  return allocation_count.load(std::memory_order_relaxed);
}

struct ReferenceBook::Impl {
  struct StoredOrder {
    OrderId order_id{};
    Side side{};
    Price price{};
    Quantity remaining_quantity{};
    AcceptanceSequence acceptance_sequence{};
  };

  using OrderQueue = std::list<StoredOrder>;
  using OrderIterator = OrderQueue::iterator;
  using BidLevels = std::map<Price, OrderQueue, std::greater<Price>>;
  using AskLevels = std::map<Price, OrderQueue>;

  struct Location {
    Side side{};
    Price price{};
    OrderIterator iterator{};
  };

  explicit Impl(const BookConfig book_config) : config(book_config) {
    if (config.price_max < 1 || config.quantity_max == 0U) {
      throw std::invalid_argument("book limits must be positive");
    }
  }

  [[nodiscard]] bool valid_price(const Price price) const noexcept {
    return price >= 1 && price <= config.price_max;
  }

  [[nodiscard]] bool valid_quantity(const Quantity quantity) const noexcept {
    return quantity > 0U && quantity <= config.quantity_max;
  }

  [[nodiscard]] std::optional<RejectReason> validate_new_order(const OrderId order_id,
                                                               const std::optional<Price> price,
                                                               const Quantity quantity) const {
    if (locations.contains(order_id)) {
      return RejectReason::DuplicateOrderId;
    }
    if (price.has_value() && !valid_price(*price)) {
      return RejectReason::InvalidPrice;
    }
    if (!valid_quantity(quantity)) {
      return RejectReason::InvalidQuantity;
    }
    return std::nullopt;
  }

  void add_resting(const OrderId order_id, const Side side, const Price price,
                   const Quantity quantity) {
    if (next_acceptance_sequence == std::numeric_limits<AcceptanceSequence>::max())
      throw std::overflow_error("acceptance sequence space is exhausted");
    StoredOrder order{.order_id = order_id,
                      .side = side,
                      .price = price,
                      .remaining_quantity = quantity,
                      .acceptance_sequence = next_acceptance_sequence++};
    insert_resting(order);
  }

  void insert_resting(const StoredOrder& order) {
    if (locations.contains(order.order_id))
      throw std::invalid_argument("snapshot contains duplicate live order ids");

    auto insert_at_level = [this, &order](auto& levels) {
      const auto [level_it, inserted_level] = levels.try_emplace(order.price);
      auto& queue = level_it->second;
      try {
        queue.push_back(order);
      } catch (...) {
        if (inserted_level) levels.erase(level_it);
        throw;
      }
      const auto order_it = std::prev(queue.end());
      try {
        locations.emplace(order.order_id, Location{order.side, order.price, order_it});
      } catch (...) {
        queue.erase(order_it);
        if (queue.empty()) levels.erase(level_it);
        throw;
      }
    };

    if (order.side == Side::Buy) {
      insert_at_level(bids);
    } else {
      insert_at_level(asks);
    }
  }

  void remove_resting(const OrderId order_id) {
    const auto location_it = locations.find(order_id);
    if (location_it == locations.end()) {
      throw std::logic_error("attempted to remove an order absent from the live index");
    }

    const Location location = location_it->second;
    if (location.side == Side::Buy) {
      const auto level_it = bids.find(location.price);
      if (level_it == bids.end()) {
        throw std::logic_error("buy index points to an absent price level");
      }
      level_it->second.erase(location.iterator);
      locations.erase(location_it);
      if (level_it->second.empty()) {
        bids.erase(level_it);
      }
      return;
    }

    const auto level_it = asks.find(location.price);
    if (level_it == asks.end()) {
      throw std::logic_error("sell index points to an absent price level");
    }
    level_it->second.erase(location.iterator);
    locations.erase(location_it);
    if (level_it->second.empty()) {
      asks.erase(level_it);
    }
  }

  void emit_fill(std::vector<Event>& events, const CommandSequence command_sequence,
                 EventIndex& event_index, const StoredOrder& maker, const OrderId taker_order_id,
                 const Quantity quantity) const {
    events.push_back(Event{.kind = EventKind::Filled,
                           .command_sequence = command_sequence,
                           .event_index = event_index++,
                           .maker_order_id = maker.order_id,
                           .taker_order_id = taker_order_id,
                           .price = maker.price,
                           .quantity = quantity});
  }

  void match_buy(std::vector<Event>& events, const CommandSequence command_sequence,
                 const OrderId taker_order_id, Quantity& remaining_quantity,
                 const std::optional<std::int64_t> limit_price, EventIndex& event_index) {
    while (remaining_quantity > 0U && !asks.empty()) {
      auto level_it = asks.begin();
      if (limit_price.has_value() && static_cast<std::int64_t>(level_it->first) > *limit_price) {
        break;
      }

      auto& maker = level_it->second.front();
      const Quantity fill_quantity = std::min(remaining_quantity, maker.remaining_quantity);
      emit_fill(events, command_sequence, event_index, maker, taker_order_id, fill_quantity);
      remaining_quantity -= fill_quantity;
      maker.remaining_quantity -= fill_quantity;
      if (maker.remaining_quantity == 0U) {
        remove_resting(maker.order_id);
      }
    }
  }

  void match_sell(std::vector<Event>& events, const CommandSequence command_sequence,
                  const OrderId taker_order_id, Quantity& remaining_quantity,
                  const std::optional<std::int64_t> limit_price, EventIndex& event_index) {
    while (remaining_quantity > 0U && !bids.empty()) {
      auto level_it = bids.begin();
      if (limit_price.has_value() && static_cast<std::int64_t>(level_it->first) < *limit_price) {
        break;
      }

      auto& maker = level_it->second.front();
      const Quantity fill_quantity = std::min(remaining_quantity, maker.remaining_quantity);
      emit_fill(events, command_sequence, event_index, maker, taker_order_id, fill_quantity);
      remaining_quantity -= fill_quantity;
      maker.remaining_quantity -= fill_quantity;
      if (maker.remaining_quantity == 0U) {
        remove_resting(maker.order_id);
      }
    }
  }

  void submit_limit(std::vector<Event>& events, const CommandSequence command_sequence,
                    const OrderId order_id, const Side side, const Price price, Quantity quantity,
                    const bool rest_remainder) {
    EventIndex event_index = 0;
    if (side == Side::Buy) {
      match_buy(events, command_sequence, order_id, quantity, price, event_index);
    } else {
      match_sell(events, command_sequence, order_id, quantity, price, event_index);
    }

    if (quantity == 0U) {
      return;
    }
    if (rest_remainder) {
      add_resting(order_id, side, price, quantity);
      events.push_back(Event{.kind = EventKind::Rested,
                             .command_sequence = command_sequence,
                             .event_index = event_index,
                             .order_id = order_id,
                             .price = price,
                             .quantity = quantity});
      return;
    }
    events.push_back(Event{.kind = EventKind::Cancelled,
                           .command_sequence = command_sequence,
                           .event_index = event_index,
                           .order_id = order_id,
                           .quantity = quantity});
  }

  void submit_market(std::vector<Event>& events, const CommandSequence command_sequence,
                     const MarketOrder& market_order) {
    if (market_order.protection_ticks < 0) {
      events.push_back(rejection(command_sequence, RejectReason::PriceOutOfBand));
      return;
    }

    const std::optional<Price> arrival_price =
        market_order.side == Side::Buy
            ? (asks.empty() ? std::nullopt : std::optional<Price>(asks.begin()->first))
            : (bids.empty() ? std::nullopt : std::optional<Price>(bids.begin()->first));

    if (!arrival_price.has_value()) {
      events.push_back(acknowledgement(command_sequence));
      events.push_back(Event{.kind = EventKind::Cancelled,
                             .command_sequence = command_sequence,
                             .event_index = 0,
                             .order_id = market_order.order_id,
                             .quantity = market_order.quantity});
      return;
    }

    const std::int64_t signed_band = static_cast<std::int64_t>(market_order.protection_ticks);
    const std::int64_t limit = market_order.side == Side::Buy
                                   ? static_cast<std::int64_t>(*arrival_price) + signed_band
                                   : static_cast<std::int64_t>(*arrival_price) - signed_band;
    events.push_back(acknowledgement(command_sequence));
    Quantity remaining_quantity = market_order.quantity;
    EventIndex event_index = 0;
    if (market_order.side == Side::Buy) {
      match_buy(events, command_sequence, market_order.order_id, remaining_quantity, limit,
                event_index);
    } else {
      match_sell(events, command_sequence, market_order.order_id, remaining_quantity, limit,
                 event_index);
    }
    if (remaining_quantity > 0U) {
      events.push_back(Event{.kind = EventKind::Cancelled,
                             .command_sequence = command_sequence,
                             .event_index = event_index,
                             .order_id = market_order.order_id,
                             .quantity = remaining_quantity});
    }
  }

  [[nodiscard]] std::vector<Event> process(const Command& command) {
    if (next_command_sequence == 0U)
      throw std::overflow_error("command sequence space is exhausted");
    if (command.sequence != next_command_sequence) {
      throw std::invalid_argument("commands must have contiguous increasing sequence numbers");
    }
    ++next_command_sequence;

    return std::visit(
        [this, sequence = command.sequence](const auto& body) -> std::vector<Event> {
          std::vector<Event> events;
          using Body = std::decay_t<decltype(body)>;
          if constexpr (std::is_same_v<Body, LimitGtc> || std::is_same_v<Body, LimitIoc>) {
            if (const auto reason = validate_new_order(body.order_id, body.price, body.quantity);
                reason.has_value()) {
              events.push_back(rejection(sequence, *reason));
              return events;
            }
            events.push_back(acknowledgement(sequence));
            submit_limit(events, sequence, body.order_id, body.side, body.price, body.quantity,
                         std::is_same_v<Body, LimitGtc>);
            return events;
          } else if constexpr (std::is_same_v<Body, MarketOrder>) {
            if (const auto reason = validate_new_order(body.order_id, std::nullopt, body.quantity);
                reason.has_value()) {
              events.push_back(rejection(sequence, *reason));
              return events;
            }
            submit_market(events, sequence, body);
            return events;
          } else if constexpr (std::is_same_v<Body, CancelOrder>) {
            const auto location_it = locations.find(body.order_id);
            if (location_it == locations.end()) {
              events.push_back(rejection(sequence, RejectReason::UnknownOrder));
              return events;
            }
            const Quantity quantity = location_it->second.iterator->remaining_quantity;
            events.push_back(acknowledgement(sequence));
            events.push_back(Event{.kind = EventKind::Cancelled,
                                   .command_sequence = sequence,
                                   .event_index = 0,
                                   .order_id = body.order_id,
                                   .quantity = quantity});
            remove_resting(body.order_id);
            return events;
          } else {
            const auto location_it = locations.find(body.order_id);
            if (location_it == locations.end()) {
              events.push_back(rejection(sequence, RejectReason::UnknownOrder));
              return events;
            }
            if (!valid_price(body.new_price)) {
              events.push_back(rejection(sequence, RejectReason::InvalidPrice));
              return events;
            }
            if (!valid_quantity(body.new_quantity)) {
              events.push_back(rejection(sequence, RejectReason::InvalidQuantity));
              return events;
            }

            const Location location = location_it->second;
            const Quantity old_quantity = location.iterator->remaining_quantity;
            events.push_back(acknowledgement(sequence));
            if (location.price == body.new_price && body.new_quantity <= old_quantity) {
              location.iterator->remaining_quantity = body.new_quantity;
              return events;
            }

            const Side side = location.side;
            remove_resting(body.order_id);
            submit_limit(events, sequence, body.order_id, side, body.new_price, body.new_quantity,
                         true);
            return events;
          }
        },
        command.body);
  }

  [[nodiscard]] std::vector<RestingOrder> resting_orders(const Side side) const {
    std::vector<RestingOrder> result;
    const auto append = [&result](const auto& levels) {
      for (const auto& [price, queue] : levels) {
        static_cast<void>(price);
        for (const StoredOrder& order : queue) {
          result.push_back(RestingOrder{.order_id = order.order_id,
                                        .side = order.side,
                                        .price = order.price,
                                        .remaining_quantity = order.remaining_quantity,
                                        .acceptance_sequence = order.acceptance_sequence});
        }
      }
    };
    if (side == Side::Buy) {
      append(bids);
    } else {
      append(asks);
    }
    return result;
  }

  [[nodiscard]] BookSnapshot snapshot() const {
    BookSnapshot result;
    result.config = config;
    result.last_command_sequence = next_command_sequence == 0U
                                       ? std::numeric_limits<CommandSequence>::max()
                                       : next_command_sequence - 1U;
    result.next_acceptance_sequence = next_acceptance_sequence;
    result.orders = resting_orders(Side::Buy);
    const auto asks_snapshot = resting_orders(Side::Sell);
    result.orders.insert(result.orders.end(), asks_snapshot.begin(), asks_snapshot.end());
    return result;
  }

  void restore(const BookSnapshot& snapshot) {
    if (snapshot.config != config)
      throw std::invalid_argument("snapshot book configuration does not match");
    if (snapshot.next_acceptance_sequence == 0U)
      throw std::invalid_argument("snapshot next acceptance sequence must be positive");
    if ((snapshot.last_command_sequence == 0U && !snapshot.orders.empty()) ||
        (snapshot.last_command_sequence != std::numeric_limits<CommandSequence>::max() &&
         snapshot.next_acceptance_sequence > snapshot.last_command_sequence + 1U))
      throw std::invalid_argument("snapshot sequence metadata is inconsistent");

    std::vector<RestingOrder> orders = snapshot.orders;
    for (const auto& order : orders) {
      if ((order.side != Side::Buy && order.side != Side::Sell) || !valid_price(order.price) ||
          !valid_quantity(order.remaining_quantity) || order.acceptance_sequence == 0U ||
          order.acceptance_sequence >= snapshot.next_acceptance_sequence) {
        throw std::invalid_argument("snapshot contains invalid resting order state");
      }
    }
    std::sort(orders.begin(), orders.end(),
              [](const RestingOrder& left, const RestingOrder& right) {
                return left.acceptance_sequence < right.acceptance_sequence;
              });
    for (std::size_t i = 1; i < orders.size(); ++i) {
      if (orders[i - 1U].acceptance_sequence == orders[i].acceptance_sequence)
        throw std::invalid_argument("snapshot contains duplicate acceptance sequences");
    }

    Impl candidate(config);
    for (const auto& order : orders) {
      candidate.insert_resting(StoredOrder{.order_id = order.order_id,
                                           .side = order.side,
                                           .price = order.price,
                                           .remaining_quantity = order.remaining_quantity,
                                           .acceptance_sequence = order.acceptance_sequence});
    }
    candidate.next_acceptance_sequence = snapshot.next_acceptance_sequence;
    candidate.next_command_sequence =
        snapshot.last_command_sequence == std::numeric_limits<CommandSequence>::max()
            ? 0U
            : snapshot.last_command_sequence + 1U;
    if (const auto violation = candidate.invariant_violation(); violation.has_value())
      throw std::invalid_argument("snapshot violates book invariants: " + *violation);
    bids.swap(candidate.bids);
    asks.swap(candidate.asks);
    locations.swap(candidate.locations);
    next_command_sequence = candidate.next_command_sequence;
    next_acceptance_sequence = candidate.next_acceptance_sequence;
  }

  [[nodiscard]] std::optional<Price> best_price(const Side side) const {
    if (side == Side::Buy) {
      return bids.empty() ? std::nullopt : std::optional<Price>(bids.begin()->first);
    }
    return asks.empty() ? std::nullopt : std::optional<Price>(asks.begin()->first);
  }

  [[nodiscard]] std::optional<std::string> invariant_violation() const {
    std::size_t observed_order_count = 0U;
    const auto check_levels = [this, &observed_order_count](
                                  const auto& levels,
                                  const Side expected_side) -> std::optional<std::string> {
      for (const auto& [price, queue] : levels) {
        if (queue.empty()) {
          return "an empty price level was retained";
        }
        AcceptanceSequence previous_sequence = 0;
        for (auto order_it = queue.begin(); order_it != queue.end(); ++order_it) {
          const StoredOrder& order = *order_it;
          ++observed_order_count;
          if (order.side != expected_side || order.price != price) {
            return "order metadata disagrees with its price level";
          }
          if (!valid_price(order.price) || !valid_quantity(order.remaining_quantity)) {
            return "resting order has invalid price or quantity";
          }
          if (previous_sequence >= order.acceptance_sequence) {
            return "FIFO acceptance sequence is not strictly increasing";
          }
          previous_sequence = order.acceptance_sequence;
          const auto location_it = locations.find(order.order_id);
          if (location_it == locations.end() || location_it->second.side != expected_side ||
              location_it->second.price != price || location_it->second.iterator != order_it) {
            return "live-order index disagrees with book storage";
          }
        }
      }
      return std::nullopt;
    };

    if (const auto failure = check_levels(bids, Side::Buy); failure.has_value()) {
      return failure;
    }
    if (const auto failure = check_levels(asks, Side::Sell); failure.has_value()) {
      return failure;
    }
    if (observed_order_count != locations.size()) {
      return "live-order index contains an order absent from the book";
    }
    if (!bids.empty() && !asks.empty() && bids.begin()->first >= asks.begin()->first) {
      return "continuous book is locked or crossed";
    }
    return std::nullopt;
  }

  BookConfig config;
  BidLevels bids;
  AskLevels asks;
  std::unordered_map<OrderId, Location> locations;
  CommandSequence next_command_sequence{1};
  AcceptanceSequence next_acceptance_sequence{1};
};

ReferenceBook::ReferenceBook(const BookConfig config) : impl_(std::make_unique<Impl>(config)) {}
ReferenceBook::~ReferenceBook() = default;
ReferenceBook::ReferenceBook(ReferenceBook&&) noexcept = default;
ReferenceBook& ReferenceBook::operator=(ReferenceBook&&) noexcept = default;

std::vector<Event> ReferenceBook::process(const Command& command) {
  return impl_->process(command);
}

std::vector<RestingOrder> ReferenceBook::resting_orders(const Side side) const {
  return impl_->resting_orders(side);
}

BookSnapshot ReferenceBook::snapshot() const {
  return impl_->snapshot();
}

void ReferenceBook::restore(const BookSnapshot& snapshot) {
  impl_->restore(snapshot);
}

std::optional<Price> ReferenceBook::best_price(const Side side) const {
  return impl_->best_price(side);
}

std::size_t ReferenceBook::live_order_count() const noexcept {
  return impl_->locations.size();
}

std::optional<std::string> ReferenceBook::invariant_violation() const {
  return impl_->invariant_violation();
}

}  // namespace chronex

#if defined(CHRONEX_ENABLE_ALLOCATION_COUNTER)
void* operator new(const std::size_t size) {
  chronex::allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept {
  std::free(memory);
}
void operator delete(void* memory, const std::size_t) noexcept {
  std::free(memory);
}
#endif
