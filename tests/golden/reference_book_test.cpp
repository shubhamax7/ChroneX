#include "chronex/oracle/reference_book.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <tuple>
#include <vector>

namespace chronex {
namespace {

class ReferenceBookTest : public ::testing::Test {
 protected:
  template <typename Body>
  std::vector<Event> submit(Body body) {
    const std::vector<Event> events =
        book_.process(Command{.sequence = next_sequence_++, .body = body});
    EXPECT_FALSE(book_.invariant_violation().has_value());
    return events;
  }

  ReferenceBook book_{BookConfig{.price_max = 200, .quantity_max = 100}};
  CommandSequence next_sequence_{1};
};

TEST_F(ReferenceBookTest, CrossingLimitUsesRestingMakerPrice) {
  submit(LimitGtc{.order_id = 1, .side = Side::Sell, .price = 101, .quantity = 7});
  const auto events =
      submit(LimitGtc{.order_id = 2, .side = Side::Buy, .price = 105, .quantity = 5});

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].kind, EventKind::Acknowledged);
  EXPECT_EQ(events[1], (Event{.kind = EventKind::Filled,
                              .command_sequence = 2,
                              .event_index = 0,
                              .maker_order_id = 1,
                              .taker_order_id = 2,
                              .price = 101,
                              .quantity = 5}));
  EXPECT_EQ(book_.resting_orders(Side::Sell)[0].remaining_quantity, 2U);
}

TEST_F(ReferenceBookTest, GtcRemainderRestsAfterFills) {
  submit(LimitGtc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 9});
  const auto events =
      submit(LimitGtc{.order_id = 2, .side = Side::Sell, .price = 99, .quantity = 12});

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[1].kind, EventKind::Filled);
  EXPECT_EQ(events[1].quantity, 9U);
  EXPECT_EQ(events[2].kind, EventKind::Rested);
  EXPECT_EQ(events[2].order_id, 2U);
  EXPECT_EQ(events[2].quantity, 3U);
}

TEST_F(ReferenceBookTest, IocRemainderCancelsAndNeverRests) {
  submit(LimitGtc{.order_id = 1, .side = Side::Sell, .price = 101, .quantity = 2});
  const auto events =
      submit(LimitIoc{.order_id = 2, .side = Side::Buy, .price = 105, .quantity = 5});

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[2].kind, EventKind::Cancelled);
  EXPECT_EQ(events[2].quantity, 3U);
  EXPECT_TRUE(book_.resting_orders(Side::Buy).empty());
}

TEST_F(ReferenceBookTest, NonCrossingIocCancelsInFull) {
  const auto events =
      submit(LimitIoc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 5});

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[1].kind, EventKind::Cancelled);
  EXPECT_EQ(events[1].quantity, 5U);
  EXPECT_EQ(book_.live_order_count(), 0U);
}

TEST_F(ReferenceBookTest, FifoAtOnePriceIsPreserved) {
  submit(LimitGtc{.order_id = 1, .side = Side::Sell, .price = 100, .quantity = 2});
  submit(LimitGtc{.order_id = 2, .side = Side::Sell, .price = 100, .quantity = 3});
  const auto events =
      submit(LimitGtc{.order_id = 3, .side = Side::Buy, .price = 100, .quantity = 4});

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[1].maker_order_id, 1U);
  EXPECT_EQ(events[1].quantity, 2U);
  EXPECT_EQ(events[2].maker_order_id, 2U);
  EXPECT_EQ(events[2].quantity, 2U);
}

TEST_F(ReferenceBookTest, BetterPricePrecedesOlderWorsePrice) {
  submit(LimitGtc{.order_id = 1, .side = Side::Sell, .price = 99, .quantity = 2});
  submit(LimitGtc{.order_id = 2, .side = Side::Sell, .price = 100, .quantity = 2});
  const auto events =
      submit(LimitGtc{.order_id = 3, .side = Side::Buy, .price = 101, .quantity = 3});

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[1].maker_order_id, 1U);
  EXPECT_EQ(events[2].maker_order_id, 2U);
}

TEST_F(ReferenceBookTest, PartialMakerRetainsPriority) {
  submit(LimitGtc{.order_id = 1, .side = Side::Sell, .price = 100, .quantity = 5});
  submit(LimitGtc{.order_id = 2, .side = Side::Sell, .price = 100, .quantity = 5});
  submit(LimitGtc{.order_id = 3, .side = Side::Buy, .price = 100, .quantity = 3});
  const auto events =
      submit(LimitGtc{.order_id = 4, .side = Side::Buy, .price = 100, .quantity = 2});

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[1].maker_order_id, 1U);
}

TEST_F(ReferenceBookTest, CancelRemovesLiveOrderAndUnknownCancelRejects) {
  submit(LimitGtc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 5});
  const auto cancelled = submit(CancelOrder{.order_id = 1});
  const auto rejected = submit(CancelOrder{.order_id = 1});

  ASSERT_EQ(cancelled.size(), 2U);
  EXPECT_EQ(cancelled[1].kind, EventKind::Cancelled);
  EXPECT_EQ(cancelled[1].quantity, 5U);
  ASSERT_EQ(rejected.size(), 1U);
  EXPECT_EQ(rejected[0].reject_reason, RejectReason::UnknownOrder);
}

TEST_F(ReferenceBookTest, DuplicateLiveIdRejectsWithoutMutatingBook) {
  submit(LimitGtc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 5});
  const auto events =
      submit(LimitGtc{.order_id = 1, .side = Side::Sell, .price = 101, .quantity = 2});

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].reject_reason, RejectReason::DuplicateOrderId);
  const auto orders = book_.resting_orders(Side::Buy);
  ASSERT_EQ(orders.size(), 1U);
  EXPECT_EQ(orders[0].remaining_quantity, 5U);
}

TEST_F(ReferenceBookTest, InvalidReplaceIsAtomic) {
  submit(LimitGtc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 5});
  const auto events = submit(ReplaceOrder{.order_id = 1, .new_price = 0, .new_quantity = 4});

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].reject_reason, RejectReason::InvalidPrice);
  const auto orders = book_.resting_orders(Side::Buy);
  ASSERT_EQ(orders.size(), 1U);
  EXPECT_EQ(orders[0].price, 100);
  EXPECT_EQ(orders[0].remaining_quantity, 5U);
}

TEST_F(ReferenceBookTest, SamePriceDecreaseKeepsPriority) {
  submit(LimitGtc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 5});
  submit(LimitGtc{.order_id = 2, .side = Side::Buy, .price = 100, .quantity = 5});
  submit(ReplaceOrder{.order_id = 1, .new_price = 100, .new_quantity = 3});
  const auto events =
      submit(LimitGtc{.order_id = 3, .side = Side::Sell, .price = 100, .quantity = 4});

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[1].maker_order_id, 1U);
  EXPECT_EQ(events[2].maker_order_id, 2U);
}

TEST_F(ReferenceBookTest, QuantityIncreaseLosesPriority) {
  submit(LimitGtc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 5});
  submit(LimitGtc{.order_id = 2, .side = Side::Buy, .price = 100, .quantity = 5});
  submit(ReplaceOrder{.order_id = 1, .new_price = 100, .new_quantity = 6});
  const auto events =
      submit(LimitGtc{.order_id = 3, .side = Side::Sell, .price = 100, .quantity = 1});

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[1].maker_order_id, 2U);
}

TEST_F(ReferenceBookTest, PriceChangingReplaceCanMatchThenRest) {
  submit(LimitGtc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 5});
  submit(LimitGtc{.order_id = 2, .side = Side::Sell, .price = 101, .quantity = 3});
  const auto events = submit(ReplaceOrder{.order_id = 1, .new_price = 101, .new_quantity = 5});

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[1].maker_order_id, 2U);
  EXPECT_EQ(events[1].price, 101);
  EXPECT_EQ(events[2].kind, EventKind::Rested);
  EXPECT_EQ(events[2].quantity, 2U);
}

TEST_F(ReferenceBookTest, MarketProtectionStopsAtArrivalBand) {
  submit(LimitGtc{.order_id = 1, .side = Side::Sell, .price = 100, .quantity = 2});
  submit(LimitGtc{.order_id = 2, .side = Side::Sell, .price = 103, .quantity = 2});
  const auto events =
      submit(MarketOrder{.order_id = 3, .side = Side::Buy, .quantity = 4, .protection_ticks = 1});

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[1].price, 100);
  EXPECT_EQ(events[2].kind, EventKind::Cancelled);
  EXPECT_EQ(events[2].quantity, 2U);
  EXPECT_EQ(book_.best_price(Side::Sell), 103);
}

TEST_F(ReferenceBookTest, EmptyBookMarketCancelsInFull) {
  const auto events =
      submit(MarketOrder{.order_id = 1, .side = Side::Buy, .quantity = 5, .protection_ticks = 2});

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[1].kind, EventKind::Cancelled);
  EXPECT_EQ(events[1].quantity, 5U);
}

class RestingLimitGoldenTest : public ::testing::TestWithParam<std::tuple<Side, Price, Quantity>> {
};

TEST_P(RestingLimitGoldenTest, ValidNonCrossingGtcProducesExactRestEvent) {
  const auto [side, price, quantity] = GetParam();
  ReferenceBook book(BookConfig{.price_max = 200, .quantity_max = 100});
  const auto events = book.process(Command{
      .sequence = 1,
      .body = LimitGtc{.order_id = 42, .side = side, .price = price, .quantity = quantity}});

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].kind, EventKind::Acknowledged);
  EXPECT_EQ(events[1], (Event{.kind = EventKind::Rested,
                              .command_sequence = 1,
                              .event_index = 0,
                              .order_id = 42,
                              .price = price,
                              .quantity = quantity}));
  EXPECT_FALSE(book.invariant_violation().has_value());
  EXPECT_EQ(book.resting_orders(side),
            std::vector<RestingOrder>({RestingOrder{.order_id = 42,
                                                    .side = side,
                                                    .price = price,
                                                    .remaining_quantity = quantity,
                                                    .acceptance_sequence = 1}}));
}

INSTANTIATE_TEST_SUITE_P(GoldenRestingLimits, RestingLimitGoldenTest,
                         ::testing::Combine(::testing::Values(Side::Buy, Side::Sell),
                                            ::testing::Values<Price>(1, 50, 100, 200),
                                            ::testing::Values<Quantity>(1U, 2U, 3U, 5U, 8U, 13U,
                                                                        21U)));

TEST(ReferenceBookDeterminismTest, SeededCommandStreamsProduceIdenticalEventsAndState) {
  constexpr std::array<std::uint32_t, 4> kSeeds{7U, 19U, 101U, 991U};
  for (const std::uint32_t seed : kSeeds) {
    std::mt19937 generator(seed);
    ReferenceBook first(BookConfig{.price_max = 200, .quantity_max = 100});
    ReferenceBook second(BookConfig{.price_max = 200, .quantity_max = 100});
    CommandSequence sequence = 1;
    OrderId next_order_id = 1;

    for (int step = 0; step < 500; ++step) {
      const auto side = (generator() % 2U == 0U) ? Side::Buy : Side::Sell;
      const auto price = static_cast<Price>(1U + (generator() % 200U));
      const auto quantity = static_cast<Quantity>(1U + (generator() % 10U));
      const auto selector = generator() % 5U;
      CommandBody body;
      if (selector == 0U) {
        body = LimitGtc{
            .order_id = next_order_id++, .side = side, .price = price, .quantity = quantity};
      } else if (selector == 1U) {
        body = LimitIoc{
            .order_id = next_order_id++, .side = side, .price = price, .quantity = quantity};
      } else if (selector == 2U) {
        body = MarketOrder{.order_id = next_order_id++,
                           .side = side,
                           .quantity = quantity,
                           .protection_ticks = static_cast<Price>(generator() % 10U)};
      } else if (selector == 3U) {
        body = CancelOrder{.order_id = 1U + (generator() % (next_order_id + 10U))};
      } else {
        body = ReplaceOrder{.order_id = 1U + (generator() % (next_order_id + 10U)),
                            .new_price = price,
                            .new_quantity = quantity};
      }

      const Command command{.sequence = sequence++, .body = body};
      EXPECT_EQ(first.process(command), second.process(command))
          << "seed=" << seed << " step=" << step;
      EXPECT_EQ(first.resting_orders(Side::Buy), second.resting_orders(Side::Buy));
      EXPECT_EQ(first.resting_orders(Side::Sell), second.resting_orders(Side::Sell));
      EXPECT_FALSE(first.invariant_violation().has_value()) << "seed=" << seed << " step=" << step;
      EXPECT_FALSE(second.invariant_violation().has_value()) << "seed=" << seed << " step=" << step;
    }
  }
}

}  // namespace
}  // namespace chronex
