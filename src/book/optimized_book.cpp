#include "chronex/book/optimized_book.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace chronex {
namespace {
constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] Event ack(const CommandSequence sequence) {
  return Event{.kind = EventKind::Acknowledged, .command_sequence = sequence};
}
[[nodiscard]] Event reject(const CommandSequence sequence, const RejectReason reason) {
  return Event{.kind = EventKind::Rejected, .command_sequence = sequence, .reject_reason = reason};
}
}  // namespace

struct OptimizedBook::Impl {
  struct Node {
    OrderId id{};
    Side side{};
    Price price{};
    Quantity remaining{};
    AcceptanceSequence accepted{};
    std::uint32_t previous{kNone};
    std::uint32_t next{kNone};
    std::uint32_t free_next{kNone};
    bool live{};
  };
  struct Level {
    std::uint32_t head{kNone};
    std::uint32_t tail{kNone};
    std::size_t count{};
  };

  explicit Impl(const OptimizedBookConfig cfg)
      : config(cfg),
        bids(static_cast<std::size_t>(cfg.limits.price_max) + 1U),
        asks(static_cast<std::size_t>(cfg.limits.price_max) + 1U),
        slots(cfg.order_capacity),
        bitmap_words((bids.size() + 63U) / 64U) {
    if (config.limits.price_max < 1 || config.limits.quantity_max == 0U ||
        config.order_capacity == 0U || config.order_capacity >= kNone) {
      throw std::invalid_argument("optimized book limits and capacity must be positive");
    }
    bid_bitmap.emplace_back(bitmap_words, 0U);
    ask_bitmap.emplace_back(bitmap_words, 0U);
    while (bid_bitmap.back().size() > 1U) {
      const auto words = (bid_bitmap.back().size() + 63U) / 64U;
      bid_bitmap.emplace_back(words, 0U);
      ask_bitmap.emplace_back(words, 0U);
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
      slots[i].free_next = i + 1U < slots.size() ? static_cast<std::uint32_t>(i + 1U) : kNone;
    }
    free_head = 0U;
  }

  [[nodiscard]] bool valid_price(Price p) const noexcept {
    return p > 0 && p <= config.limits.price_max;
  }
  [[nodiscard]] bool valid_qty(Quantity q) const noexcept {
    return q > 0U && q <= config.limits.quantity_max;
  }
  [[nodiscard]] std::optional<RejectReason> validate_id_price_qty(OrderId id,
                                                                  std::optional<Price> price,
                                                                  Quantity qty) const {
    if (locations.contains(id)) return RejectReason::DuplicateOrderId;
    if (price && !valid_price(*price)) return RejectReason::InvalidPrice;
    if (!valid_qty(qty)) return RejectReason::InvalidQuantity;
    return std::nullopt;
  }

  [[nodiscard]] std::vector<std::vector<std::uint64_t>>& bits(Side s) {
    return s == Side::Buy ? bid_bitmap : ask_bitmap;
  }
  [[nodiscard]] const std::vector<std::vector<std::uint64_t>>& bits(Side s) const {
    return s == Side::Buy ? bid_bitmap : ask_bitmap;
  }
  [[nodiscard]] std::vector<Level>& levels(Side s) {
    return s == Side::Buy ? bids : asks;
  }
  [[nodiscard]] const std::vector<Level>& levels(Side s) const {
    return s == Side::Buy ? bids : asks;
  }
  [[nodiscard]] std::optional<Price> best(Side s) const {
    const auto& hierarchy = bits(s);
    const bool highest = s == Side::Buy;
    std::size_t word_index = 0U;
    for (std::size_t level = hierarchy.size(); level-- > 0U;) {
      const auto word = hierarchy[level][word_index];
      if (word == 0U) return std::nullopt;
      const auto bit = highest ? 63U - static_cast<std::size_t>(std::countl_zero(word))
                               : static_cast<std::size_t>(std::countr_zero(word));
      word_index = word_index * 64U + bit;
    }
    return static_cast<Price>(word_index);
  }
  void set_bit(Side s, Price p) {
    auto& hierarchy = bits(s);
    std::size_t index = static_cast<std::size_t>(p);
    for (auto& level : hierarchy) {
      const auto word_index = index / 64U;
      const auto mask = 1ULL << (index % 64U);
      const bool was_empty = level[word_index] == 0U;
      level[word_index] |= mask;
      if (!was_empty) break;
      index = word_index;
    }
  }
  void clear_bit(Side s, Price p) {
    auto& hierarchy = bits(s);
    std::size_t index = static_cast<std::size_t>(p);
    for (auto& level : hierarchy) {
      const auto word_index = index / 64U;
      auto& word = level[word_index];
      word &= ~(1ULL << (index % 64U));
      if (word != 0U) break;
      index = word_index;
    }
  }

  [[nodiscard]] std::uint32_t allocate_node(OrderId id, Side side, Price price, Quantity qty) {
    if (free_head == kNone) return kNone;
    const auto index = free_head;
    Node& node = slots[index];
    free_head = node.free_next;
    node = Node{.id = id,
                .side = side,
                .price = price,
                .remaining = qty,
                .accepted = next_acceptance++,
                .previous = kNone,
                .next = kNone,
                .free_next = kNone,
                .live = true};
    return index;
  }
  void free_node(std::uint32_t index) {
    Node& n = slots[index];
    n.live = false;
    n.free_next = free_head;
    free_head = index;
  }
  bool add(OrderId id, Side side, Price price, Quantity qty) {
    const auto index = allocate_node(id, side, price, qty);
    if (index == kNone) return false;
    auto& level = levels(side)[static_cast<std::size_t>(price)];
    if (level.tail == kNone) {
      level.head = index;
      set_bit(side, price);
    } else {
      slots[level.tail].next = index;
      slots[index].previous = level.tail;
    }
    level.tail = index;
    ++level.count;
    locations.emplace(id, index);
    return true;
  }
  void unlink(std::uint32_t index) {
    Node& node = slots[index];
    auto& level = levels(node.side)[static_cast<std::size_t>(node.price)];
    if (node.previous == kNone)
      level.head = node.next;
    else
      slots[node.previous].next = node.next;
    if (node.next == kNone)
      level.tail = node.previous;
    else
      slots[node.next].previous = node.previous;
    --level.count;
    if (level.count == 0U) clear_bit(node.side, node.price);
    locations.erase(node.id);
    free_node(index);
  }
  void fill_events(std::vector<Event>& out, CommandSequence seq, EventIndex& idx, OrderId taker,
                   Quantity& remaining, Side aggressor, std::optional<std::int64_t> limit) {
    while (remaining > 0U) {
      const auto bp = best(opposite(aggressor));
      if (!bp) break;
      if (limit && (aggressor == Side::Buy ? static_cast<std::int64_t>(*bp) > *limit
                                           : static_cast<std::int64_t>(*bp) < *limit))
        break;
      auto& level = levels(opposite(aggressor))[static_cast<std::size_t>(*bp)];
      const auto mi = level.head;
      Node& maker = slots[mi];
      const auto q = std::min(remaining, maker.remaining);
      out.push_back(Event{.kind = EventKind::Filled,
                          .command_sequence = seq,
                          .event_index = idx++,
                          .maker_order_id = maker.id,
                          .taker_order_id = taker,
                          .price = maker.price,
                          .quantity = q});
      remaining -= q;
      maker.remaining -= q;
      if (maker.remaining == 0U) unlink(mi);
    }
  }
  [[nodiscard]] bool would_rest(Side side, Price price, Quantity quantity) const {
    while (quantity > 0U) {
      const auto bp = best(opposite(side));
      if (!bp) return true;
      if ((side == Side::Buy && *bp > price) || (side == Side::Sell && *bp < price)) return true;
      const auto& level = levels(opposite(side))[static_cast<std::size_t>(*bp)];
      for (auto i = level.head; i != kNone && quantity > 0U; i = slots[i].next) {
        if (slots[i].remaining >= quantity) return false;
        quantity -= slots[i].remaining;
      }
    }
    return false;
  }
  void submit_limit(std::vector<Event>& out, CommandSequence seq, OrderId id, Side side,
                    Price price, Quantity qty, bool gtc) {
    EventIndex idx = 0;
    fill_events(out, seq, idx, id, qty, side, price);
    if (qty == 0U) return;
    if (gtc && add(id, side, price, qty)) {
      out.push_back(Event{.kind = EventKind::Rested,
                          .command_sequence = seq,
                          .event_index = idx,
                          .order_id = id,
                          .price = price,
                          .quantity = qty});
    } else {
      out.push_back(Event{.kind = EventKind::Cancelled,
                          .command_sequence = seq,
                          .event_index = idx,
                          .order_id = id,
                          .quantity = qty});
    }
  }
  std::vector<Event> process(const Command& cmd) {
    if (cmd.sequence != next_command)
      throw std::invalid_argument("commands must have contiguous sequence numbers");
    ++next_command;
    return std::visit(
        [&](const auto& b) -> std::vector<Event> {
          using T = std::decay_t<decltype(b)>;
          std::vector<Event> out;
          if constexpr (std::is_same_v<T, LimitGtc> || std::is_same_v<T, LimitIoc>) {
            if (auto r = validate_id_price_qty(b.order_id, b.price, b.quantity)) {
              out.push_back(reject(cmd.sequence, *r));
              return out;
            }
            if constexpr (std::is_same_v<T, LimitGtc>) {
              // Only resting remainders consume a pool slot; reject before fills
              // when the command would otherwise mutate a full pool.
              if (free_head == kNone && would_rest(b.side, b.price, b.quantity)) {
                out.push_back(reject(cmd.sequence, RejectReason::CapacityExceeded));
                return out;
              }
            }
            out.push_back(ack(cmd.sequence));
            submit_limit(out, cmd.sequence, b.order_id, b.side, b.price, b.quantity,
                         std::is_same_v<T, LimitGtc>);
            return out;
          } else if constexpr (std::is_same_v<T, MarketOrder>) {
            if (auto r = validate_id_price_qty(b.order_id, std::nullopt, b.quantity)) {
              out.push_back(reject(cmd.sequence, *r));
              return out;
            }
            if (b.protection_ticks < 0) {
              out.push_back(reject(cmd.sequence, RejectReason::PriceOutOfBand));
              return out;
            }
            out.push_back(ack(cmd.sequence));
            const auto arrival = best(opposite(b.side));
            if (!arrival) {
              out.push_back(Event{.kind = EventKind::Cancelled,
                                  .command_sequence = cmd.sequence,
                                  .event_index = 0,
                                  .order_id = b.order_id,
                                  .quantity = b.quantity});
              return out;
            }
            const std::int64_t limit =
                b.side == Side::Buy ? static_cast<std::int64_t>(*arrival) + b.protection_ticks
                                    : static_cast<std::int64_t>(*arrival) - b.protection_ticks;
            Quantity q = b.quantity;
            EventIndex idx = 0;
            fill_events(out, cmd.sequence, idx, b.order_id, q, b.side, limit);
            if (q)
              out.push_back(Event{.kind = EventKind::Cancelled,
                                  .command_sequence = cmd.sequence,
                                  .event_index = idx,
                                  .order_id = b.order_id,
                                  .quantity = q});
            return out;
          } else if constexpr (std::is_same_v<T, CancelOrder>) {
            auto it = locations.find(b.order_id);
            if (it == locations.end()) {
              out.push_back(reject(cmd.sequence, RejectReason::UnknownOrder));
              return out;
            }
            auto i = it->second;
            auto q = slots[i].remaining;
            out.push_back(ack(cmd.sequence));
            out.push_back(Event{.kind = EventKind::Cancelled,
                                .command_sequence = cmd.sequence,
                                .event_index = 0,
                                .order_id = b.order_id,
                                .quantity = q});
            unlink(i);
            return out;
          } else {
            auto it = locations.find(b.order_id);
            if (it == locations.end()) {
              out.push_back(reject(cmd.sequence, RejectReason::UnknownOrder));
              return out;
            }
            if (!valid_price(b.new_price)) {
              out.push_back(reject(cmd.sequence, RejectReason::InvalidPrice));
              return out;
            }
            if (!valid_qty(b.new_quantity)) {
              out.push_back(reject(cmd.sequence, RejectReason::InvalidQuantity));
              return out;
            }
            auto i = it->second;
            Node old = slots[i];
            out.push_back(ack(cmd.sequence));
            if (old.price == b.new_price && b.new_quantity <= old.remaining) {
              slots[i].remaining = b.new_quantity;
              return out;
            }
            unlink(i);
            submit_limit(out, cmd.sequence, b.order_id, old.side, b.new_price, b.new_quantity,
                         true);
            return out;
          }
        },
        cmd.body);
  }
  std::vector<RestingOrder> snapshot(Side s) const {
    std::vector<RestingOrder> out;
    const auto& ls = levels(s);
    const auto append_level = [&](std::size_t p) {
      for (auto i = ls[p].head; i != kNone; i = slots[i].next) {
        const auto& n = slots[i];
        out.push_back({n.id, n.side, n.price, n.remaining, n.accepted});
      }
    };
    if (s == Side::Buy) {
      for (std::size_t p = ls.size(); p-- > 1U;) append_level(p);
    } else {
      for (std::size_t p = 1; p < ls.size(); ++p) append_level(p);
    }
    return out;
  }
  std::optional<std::string> check() const {
    std::size_t n = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
      const auto& ls = levels(side);
      for (std::size_t p = 1; p < ls.size(); ++p) {
        const auto& l = ls[p];
        std::size_t count = 0;
        std::uint32_t prev = kNone;
        AcceptanceSequence seq = 0;
        for (auto i = l.head; i != kNone; i = slots[i].next) {
          const auto& x = slots[i];
          ++count;
          ++n;
          if (!x.live || x.side != side || static_cast<std::size_t>(x.price) != p ||
              x.remaining == 0U || x.previous != prev || x.accepted <= seq)
            return "invalid pooled order or FIFO links";
          auto it = locations.find(x.id);
          if (it == locations.end() || it->second != i) return "ID index mismatch";
          prev = i;
          seq = x.accepted;
        }
        if (count != l.count || (count == 0U) != (l.head == kNone) ||
            (count == 0U) != (l.tail == kNone))
          return "price level count mismatch";
        const bool marked = (bits(side)[0][p / 64U] & (1ULL << (p % 64U))) != 0U;
        if (marked != (count > 0U)) return "price bitmap mismatch";
      }
    }
    if (n != locations.size()) return "pooled storage and ID index differ";
    auto bid = best(Side::Buy), ask = best(Side::Sell);
    if (bid && ask && *bid >= *ask) return "continuous book is locked or crossed";
    return std::nullopt;
  }
  OptimizedBookConfig config;
  std::vector<Level> bids, asks;
  std::vector<Node> slots;
  std::vector<std::vector<std::uint64_t>> bid_bitmap, ask_bitmap;
  std::size_t bitmap_words{};
  std::unordered_map<OrderId, std::uint32_t> locations;
  std::uint32_t free_head{kNone};
  CommandSequence next_command{1};
  AcceptanceSequence next_acceptance{1};
};

OptimizedBook::OptimizedBook(OptimizedBookConfig c) : impl_(std::make_unique<Impl>(c)) {}
OptimizedBook::~OptimizedBook() = default;
OptimizedBook::OptimizedBook(OptimizedBook&&) noexcept = default;
OptimizedBook& OptimizedBook::operator=(OptimizedBook&&) noexcept = default;
std::vector<Event> OptimizedBook::process(const Command& c) {
  return impl_->process(c);
}
std::vector<RestingOrder> OptimizedBook::resting_orders(Side s) const {
  return impl_->snapshot(s);
}
std::optional<Price> OptimizedBook::best_price(Side s) const {
  return impl_->best(s);
}
std::size_t OptimizedBook::live_order_count() const noexcept {
  return impl_->locations.size();
}
std::optional<std::string> OptimizedBook::invariant_violation() const {
  return impl_->check();
}
}  // namespace chronex
