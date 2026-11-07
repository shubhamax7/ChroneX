#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>

#include "chronex/book/optimized_book.hpp"

namespace chronex {
namespace {

TEST(OptimizedBookDifferentialTest, SeededStreamsMatchReferenceEventsAndState) {
  constexpr std::array<std::uint32_t, 8> seeds{3U, 7U, 19U, 101U, 991U, 4099U, 8191U, 65521U};
  for (const auto seed : seeds) {
    ReferenceBook reference(BookConfig{.price_max = 200, .quantity_max = 100});
    OptimizedBook optimized(OptimizedBookConfig{.limits = {.price_max = 200, .quantity_max = 100},
                                                .order_capacity = 2'000});
    std::mt19937 rng(seed);
    CommandSequence sequence = 1;
    OrderId new_id = 1;
    for (int step = 0; step < 5'000; ++step) {
      const auto side = (rng() & 1U) == 0U ? Side::Buy : Side::Sell;
      const auto price = static_cast<Price>(1U + rng() % 200U);
      const auto qty = static_cast<Quantity>(1U + rng() % 12U);
      const auto choice = rng() % 6U;
      CommandBody body;
      switch (choice) {
        case 0U:
          body = LimitGtc{new_id++, side, price, qty};
          break;
        case 1U:
          body = LimitIoc{new_id++, side, price, qty};
          break;
        case 2U:
          body = MarketOrder{new_id++, side, qty, static_cast<Price>(rng() % 8U)};
          break;
        case 3U:
          body = CancelOrder{1U + rng() % new_id};
          break;
        case 4U:
          body = ReplaceOrder{1U + rng() % new_id, price, qty};
          break;
        default:
          body = LimitGtc{new_id++, side, price, qty};
          break;
      }
      const Command cmd{sequence++, body};
      const auto expected = reference.process(cmd);
      const auto actual = optimized.process(cmd);
      EXPECT_EQ(actual, expected) << "seed=" << seed << " step=" << step;
      EXPECT_EQ(optimized.resting_orders(Side::Buy), reference.resting_orders(Side::Buy));
      EXPECT_EQ(optimized.resting_orders(Side::Sell), reference.resting_orders(Side::Sell));
      EXPECT_EQ(optimized.best_price(Side::Buy), reference.best_price(Side::Buy));
      EXPECT_EQ(optimized.best_price(Side::Sell), reference.best_price(Side::Sell));
      EXPECT_FALSE(optimized.invariant_violation().has_value())
          << "seed=" << seed << " step=" << step;
    }
  }
}

TEST(OptimizedBookDifferentialTest, PoolExhaustionRejectsWithoutChangingState) {
  ReferenceBook reference(BookConfig{.price_max = 10, .quantity_max = 10});
  OptimizedBook optimized(
      OptimizedBookConfig{.limits = {.price_max = 10, .quantity_max = 10}, .order_capacity = 1});
  Command first{1, LimitGtc{1, Side::Buy, 5, 2}};
  EXPECT_EQ(optimized.process(first), reference.process(first));
  const auto snapshot = optimized.resting_orders(Side::Buy);
  Command second{2, LimitGtc{2, Side::Buy, 4, 2}};
  const auto result = optimized.process(second);
  EXPECT_EQ(result.size(), 1U);
  EXPECT_EQ(result.front().reject_reason, RejectReason::CapacityExceeded);
  EXPECT_EQ(optimized.resting_orders(Side::Buy), snapshot);
  EXPECT_FALSE(optimized.invariant_violation().has_value());
}

}  // namespace
}  // namespace chronex
