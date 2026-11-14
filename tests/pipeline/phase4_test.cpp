#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

#include "chronex/journal/command_journal.hpp"
#include "chronex/market_data/depth_reconstructor.hpp"
#include "chronex/pipeline/journaled_matching_pipeline.hpp"
#include "chronex/pipeline/matching_pipeline.hpp"

namespace chronex {
namespace {

TEST(SpscRingTest, ReportsFullAndEmptyWithoutLosingOrder) {
  SpscRing<int, 2> queue;
  EXPECT_FALSE(queue.try_pop());
  EXPECT_TRUE(queue.try_push(10));
  EXPECT_TRUE(queue.try_push(20));
  EXPECT_FALSE(queue.try_push(30));
  ASSERT_TRUE(queue.try_pop());
  EXPECT_EQ(*queue.try_pop(), 20);
  EXPECT_TRUE(queue.try_push(30));
  EXPECT_EQ(*queue.try_pop(), 30);
}

TEST(SpscRingTest, PreservesOrderAcrossProducerAndConsumerThreads) {
  constexpr std::uint64_t kCount = 1'000'000;
  SpscRing<std::uint64_t, 128> queue;
  std::thread producer([&] {
    for (std::uint64_t value = 0; value < kCount; ++value)
      while (!queue.try_push(std::move(value))) std::this_thread::yield();
  });
  for (std::uint64_t expected = 0; expected < kCount;) {
    if (const auto value = queue.try_pop()) {
      EXPECT_EQ(*value, expected);
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
}

TEST(MatchingPipelineTest, BackpressureAndShutdownPreserveAcceptedCommandOrder) {
  constexpr CommandSequence kCommandCount = 5'000;
  MatchingPipeline<2> pipeline(BookConfig{.price_max = 10'000, .quantity_max = 1'000});
  std::vector<CommandSequence> seen;
  seen.reserve(static_cast<std::size_t>(kCommandCount));
  for (CommandSequence sequence = 1; sequence <= kCommandCount; ++sequence) {
    Command command{.sequence = sequence,
                    .body = LimitGtc{.order_id = sequence,
                                     .side = Side::Buy,
                                     .price = static_cast<Price>(10'000 - sequence),
                                     .quantity = 1}};
    while (!pipeline.try_submit(std::move(command))) {
      if (auto result = pipeline.try_receive())
        seen.push_back(result->sequence);
      else
        std::this_thread::yield();
      command = Command{.sequence = sequence,
                        .body = LimitGtc{.order_id = sequence,
                                         .side = Side::Buy,
                                         .price = static_cast<Price>(10'000 - sequence),
                                         .quantity = 1}};
    }
    while (auto result = pipeline.try_receive()) seen.push_back(result->sequence);
  }
  auto final_results = pipeline.drain_and_stop();
  for (auto& result : final_results) seen.push_back(result.sequence);
  ASSERT_EQ(seen.size(), static_cast<std::size_t>(kCommandCount));
  for (std::size_t i = 0; i < seen.size(); ++i) EXPECT_EQ(seen[i], i + 1U);
  EXPECT_FALSE(pipeline.try_submit(Command{}));
}

TEST(MatchingPipelineTest, ShutdownDrainsAWorkerBlockedByFullResultQueue) {
  MatchingPipeline<4> pipeline(BookConfig{.price_max = 100'000, .quantity_max = 1'000});
  std::size_t accepted = 0U;
  constexpr std::size_t kOutstandingCapacity = 4U + 4U + 1U;
  while (accepted < kOutstandingCapacity) {
    const auto sequence = static_cast<CommandSequence>(accepted + 1U);
    const Command command{.sequence = sequence,
                          .body = LimitGtc{.order_id = sequence,
                                           .side = Side::Buy,
                                           .price = static_cast<Price>(100'000 - sequence),
                                           .quantity = 1}};
    if (!pipeline.try_submit(command)) {
      std::this_thread::yield();
      continue;
    }
    ++accepted;
  }
  ASSERT_GT(accepted, 4U);
  const auto results = pipeline.drain_and_stop();
  ASSERT_EQ(results.size(), accepted);
  for (std::size_t i = 0; i < results.size(); ++i) {
    EXPECT_EQ(results[i].sequence, i + 1U);
    ASSERT_EQ(results[i].events.size(), 2U);
    EXPECT_EQ(results[i].events.front().kind, EventKind::Acknowledged);
    EXPECT_EQ(results[i].events.back().kind, EventKind::Rested);
  }
}

class JournalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("chronex-phase4-test-" + std::to_string(nonce) + ".cxj");
    std::filesystem::remove(path_);
  }
  void TearDown() override {
    std::filesystem::remove(path_);
  }
  std::filesystem::path path_;
};

TEST_F(JournalTest, EncodesEveryCommandAndReplaysIdenticalEventsAndState) {
  const std::vector<Command> commands{
      {1, LimitGtc{.order_id = 1, .side = Side::Sell, .price = 101, .quantity = 5}},
      {2, LimitIoc{.order_id = 2, .side = Side::Buy, .price = 101, .quantity = 2}},
      {3, MarketOrder{.order_id = 3, .side = Side::Buy, .quantity = 1, .protection_ticks = 0}},
      {4, CancelOrder{.order_id = 1}},
      {5, LimitGtc{.order_id = 5, .side = Side::Buy, .price = 100, .quantity = 2}},
      {6, ReplaceOrder{.order_id = 5, .new_price = 100, .new_quantity = 1}},
  };
  {
    CommandJournal journal(path_);
    for (const auto& command : commands) journal.append(command);
  }
  EXPECT_EQ(CommandJournal::recover(path_).size(), commands.size());
  const auto direct = replay_commands(commands);
  const auto recovered = replay_journal(path_);
  EXPECT_EQ(recovered.command_events, direct.command_events);
  EXPECT_EQ(recovered.bids, direct.bids);
  EXPECT_EQ(recovered.asks, direct.asks);

  ReferenceBook recovered_book;
  EXPECT_EQ(CommandJournal::replay_into(path_, recovered_book), commands.back().sequence);
  EXPECT_EQ(recovered_book.resting_orders(Side::Buy), direct.bids);
  EXPECT_EQ(recovered_book.resting_orders(Side::Sell), direct.asks);
}

TEST_F(JournalTest, TruncatesTornTailAndContinuesAtNextSequence) {
  {
    CommandJournal journal(path_);
    journal.append(
        Command{1, LimitGtc{.order_id = 1, .side = Side::Buy, .price = 10, .quantity = 2}});
  }
  {
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    output.write("CXJ", 3);
  }
  ASSERT_EQ(CommandJournal::recover(path_).size(), 1U);
  {
    CommandJournal journal(path_);
    journal.append(Command{2, CancelOrder{.order_id = 1}});
  }
  EXPECT_EQ(CommandJournal::recover(path_).size(), 2U);
}

TEST_F(JournalTest, RecoversFromEveryIncompleteFrameByteBoundary) {
  const Command command{1, LimitGtc{.order_id = 1, .side = Side::Buy, .price = 42, .quantity = 7}};
  {
    CommandJournal journal(path_);
    journal.append(command);
  }
  std::ifstream source(path_, std::ios::binary);
  const std::vector<char> complete_frame((std::istreambuf_iterator<char>(source)),
                                         std::istreambuf_iterator<char>());
  source.close();
  ASSERT_GT(complete_frame.size(), 12U);

  for (std::size_t prefix_size = 0U; prefix_size < complete_frame.size(); ++prefix_size) {
    {
      std::ofstream torn(path_, std::ios::binary | std::ios::trunc);
      torn.write(complete_frame.data(), static_cast<std::streamsize>(prefix_size));
    }
    {
      CommandJournal recovered_writer(path_);
      EXPECT_TRUE(CommandJournal::recover(path_).empty()) << "prefix bytes: " << prefix_size;
      recovered_writer.append(command);
    }
    EXPECT_EQ(CommandJournal::recover(path_).size(), 1U) << "prefix bytes: " << prefix_size;
  }
}

TEST_F(JournalTest, SyncedJournalRequiresAnExistingParentDirectory) {
  const auto nested_path = path_.parent_path() / "chronex-missing-parent" / path_.filename();
  EXPECT_THROW(CommandJournal journal(nested_path), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(nested_path.parent_path()));
}

TEST_F(JournalTest, RejectsChecksumCorruption) {
  {
    CommandJournal journal(path_);
    journal.append(
        Command{1, LimitGtc{.order_id = 1, .side = Side::Buy, .price = 10, .quantity = 2}});
  }
  std::fstream file(path_, std::ios::binary | std::ios::in | std::ios::out);
  file.seekg(12);
  char byte{};
  file.read(&byte, 1);
  byte ^= 0x1;
  file.seekp(12);
  file.write(&byte, 1);
  file.close();
  EXPECT_THROW(static_cast<void>(CommandJournal::recover(path_)), std::runtime_error);
}

TEST_F(JournalTest, JournaledPipelineReplaysStateBeforeAcceptingNextCommand) {
  {
    JournaledMatchingPipeline<4> pipeline(path_,
                                          BookConfig{.price_max = 1'000, .quantity_max = 1'000});
    ASSERT_TRUE(pipeline.try_submit(
        Command{1, LimitGtc{.order_id = 1, .side = Side::Buy, .price = 100, .quantity = 3}}));
    const auto results = pipeline.drain_and_stop();
    ASSERT_EQ(results.size(), 1U);
    ASSERT_EQ(results[0].events.size(), 2U);
    EXPECT_EQ(results[0].events[1].kind, EventKind::Rested);
  }
  {
    JournaledMatchingPipeline<4> recovered(path_,
                                           BookConfig{.price_max = 1'000, .quantity_max = 1'000});
    ASSERT_TRUE(recovered.try_submit(
        Command{2, LimitIoc{.order_id = 2, .side = Side::Sell, .price = 99, .quantity = 2}}));
    const auto results = recovered.drain_and_stop();
    ASSERT_EQ(results.size(), 1U);
    ASSERT_EQ(results[0].events.size(), 2U);
    EXPECT_EQ(results[0].events[1].kind, EventKind::Filled);
    EXPECT_EQ(results[0].events[1].maker_order_id, 1U);
  }
}

TEST(DepthReconstructorTest, AppliesAbsoluteUpdatesAndTracksBestLevels) {
  DepthReconstructor depth(1'000, 10'000);
  depth.apply_snapshot(DepthSnapshot{
      .sequence = 10,
      .levels = {
          {Side::Buy, 99, 20}, {Side::Buy, 98, 15}, {Side::Sell, 101, 12}, {Side::Sell, 102, 9}}});
  depth.apply_update(DepthUpdate{.sequence = 11, .side = Side::Buy, .price = 99, .quantity = 17});
  depth.apply_update(DepthUpdate{.sequence = 12, .side = Side::Sell, .price = 101, .quantity = 0});
  const auto view = depth.view();
  EXPECT_EQ(view.sequence, 12U);
  EXPECT_EQ(view.best_bid, 99);
  EXPECT_EQ(view.best_ask, 102);
  EXPECT_EQ(view.levels[0], (DepthLevel{Side::Buy, 99, 17}));
}

TEST(DepthReconstructorTest, RejectsGapsCrossesAndInvalidSnapshotsAtomically) {
  DepthReconstructor depth(1'000, 1'000);
  depth.apply_snapshot(
      DepthSnapshot{.sequence = 4, .levels = {{Side::Buy, 99, 5}, {Side::Sell, 101, 6}}});
  const auto before = depth.view();
  EXPECT_THROW(
      depth.apply_update(DepthUpdate{.sequence = 6, .side = Side::Buy, .price = 98, .quantity = 5}),
      std::invalid_argument);
  EXPECT_THROW(depth.apply_update(
                   DepthUpdate{.sequence = 5, .side = Side::Buy, .price = 101, .quantity = 5}),
               std::invalid_argument);
  EXPECT_EQ(depth.view().levels, before.levels);
  EXPECT_EQ(depth.view().sequence, before.sequence);
  EXPECT_THROW(depth.apply_snapshot(DepthSnapshot{
                   .sequence = 7, .levels = {{Side::Buy, 99, 5}, {Side::Sell, 99, 4}}}),
               std::invalid_argument);
  EXPECT_EQ(depth.view().levels, before.levels);
}

}  // namespace
}  // namespace chronex
