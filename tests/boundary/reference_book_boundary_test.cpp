#include <gtest/gtest.h>

#include <stdexcept>

#include "chronex/oracle/reference_book.hpp"

namespace chronex {
namespace {

TEST(ReferenceBookBoundaryTest, RejectsZeroAndOutOfRangePrices) {
  ReferenceBook book(BookConfig{.price_max = 10, .quantity_max = 10});
  const auto zero = book.process(
      Command{.sequence = 1,
              .body = LimitGtc{.order_id = 1, .side = Side::Buy, .price = 0, .quantity = 1}});
  const auto high = book.process(
      Command{.sequence = 2,
              .body = LimitGtc{.order_id = 2, .side = Side::Buy, .price = 11, .quantity = 1}});

  EXPECT_EQ(zero[0].reject_reason, RejectReason::InvalidPrice);
  EXPECT_EQ(high[0].reject_reason, RejectReason::InvalidPrice);
  EXPECT_EQ(book.live_order_count(), 0U);
}

TEST(ReferenceBookBoundaryTest, RejectsZeroAndOutOfRangeQuantity) {
  ReferenceBook book(BookConfig{.price_max = 10, .quantity_max = 10});
  const auto zero = book.process(
      Command{.sequence = 1,
              .body = LimitGtc{.order_id = 1, .side = Side::Buy, .price = 1, .quantity = 0}});
  const auto high = book.process(
      Command{.sequence = 2,
              .body = LimitGtc{.order_id = 2, .side = Side::Buy, .price = 1, .quantity = 11}});

  EXPECT_EQ(zero[0].reject_reason, RejectReason::InvalidQuantity);
  EXPECT_EQ(high[0].reject_reason, RejectReason::InvalidQuantity);
  EXPECT_EQ(book.live_order_count(), 0U);
}

TEST(ReferenceBookBoundaryTest, RejectsNegativeMarketProtectionBand) {
  ReferenceBook book;
  const auto events = book.process(Command{
      .sequence = 1,
      .body =
          MarketOrder{.order_id = 1, .side = Side::Buy, .quantity = 1, .protection_ticks = -1}});

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].reject_reason, RejectReason::PriceOutOfBand);
}

TEST(ReferenceBookBoundaryTest, RejectsUnknownReplaceWithoutMutation) {
  ReferenceBook book;
  const auto events = book.process(Command{
      .sequence = 1, .body = ReplaceOrder{.order_id = 88, .new_price = 100, .new_quantity = 1}});

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].reject_reason, RejectReason::UnknownOrder);
  EXPECT_EQ(book.live_order_count(), 0U);
}

TEST(ReferenceBookBoundaryTest, RejectsInvalidBookConfiguration) {
  EXPECT_THROW((ReferenceBook{BookConfig{.price_max = 0, .quantity_max = 1}}),
               std::invalid_argument);
  EXPECT_THROW((ReferenceBook{BookConfig{.price_max = 1, .quantity_max = 0}}),
               std::invalid_argument);
}

TEST(ReferenceBookBoundaryTest, RejectsNonContiguousCommandSequences) {
  ReferenceBook book;
  const auto process_non_contiguous_command = [&book] {
    static_cast<void>(book.process(
        Command{.sequence = 2,
                .body = LimitGtc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 1}}));
  };
  EXPECT_THROW(process_non_contiguous_command(), std::invalid_argument);
}

}  // namespace
}  // namespace chronex
