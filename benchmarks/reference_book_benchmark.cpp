#include <benchmark/benchmark.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "chronex/book/optimized_book.hpp"
#include "chronex/oracle/reference_book.hpp"
#include "chronex/pipeline/matching_pipeline.hpp"

namespace chronex {
namespace {

using BenchmarkCommand = CommandBody;

[[nodiscard]] std::vector<BenchmarkCommand> make_balanced_workload(const std::size_t count) {
  std::vector<BenchmarkCommand> workload;
  workload.reserve(count);
  for (std::size_t index = 0; index < count / 2U; ++index) {
    const auto price = static_cast<Price>(100'000U + (index % 10'000U));
    workload.emplace_back(LimitGtc{.order_id = static_cast<OrderId>(index + 1U),
                                   .side = Side::Buy,
                                   .price = price,
                                   .quantity = 5U});
    workload.emplace_back(LimitGtc{.order_id = static_cast<OrderId>(count + index + 1U),
                                   .side = Side::Sell,
                                   .price = static_cast<Price>(price + 1),
                                   .quantity = 5U});
  }
  return workload;
}

void BM_ReferenceBookBalancedAdds(benchmark::State& state) {
  const auto workload = make_balanced_workload(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    state.PauseTiming();
    ReferenceBook book(BookConfig{.price_max = 1'000'000, .quantity_max = 1'000'000});
    state.ResumeTiming();
    CommandSequence sequence = 1;
    for (const auto& body : workload) {
      const auto events = book.process(Command{.sequence = sequence++, .body = body});
      benchmark::DoNotOptimize(events.data());
      benchmark::ClobberMemory();
    }
    benchmark::DoNotOptimize(book.live_order_count());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(workload.size()));
  state.SetLabel("balanced non-crossing GTC adds");
}
BENCHMARK(BM_ReferenceBookBalancedAdds)->Arg(2'000)->Arg(20'000)->Unit(benchmark::kMillisecond);

void BM_OptimizedBookBalancedAdds(benchmark::State& state) {
  const auto workload = make_balanced_workload(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    state.PauseTiming();
    OptimizedBook book(
        OptimizedBookConfig{.limits = BookConfig{.price_max = 1'000'000, .quantity_max = 1'000'000},
                            .order_capacity = static_cast<std::size_t>(state.range(0))});
    state.ResumeTiming();
    CommandSequence sequence = 1;
    for (const auto& body : workload) {
      const auto events = book.process(Command{.sequence = sequence++, .body = body});
      benchmark::DoNotOptimize(events.data());
      benchmark::ClobberMemory();
    }
    benchmark::DoNotOptimize(book.live_order_count());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(workload.size()));
  state.SetLabel("optimized bitmap book: balanced non-crossing GTC adds");
}
BENCHMARK(BM_OptimizedBookBalancedAdds)->Arg(2'000)->Arg(20'000)->Unit(benchmark::kMillisecond);

void BM_ReferenceBookCrossingIoc(benchmark::State& state) {
  constexpr std::size_t kOrdersPerBatch = 2'000;
  for (auto _ : state) {
    ReferenceBook book(BookConfig{.price_max = 1'000'000, .quantity_max = 1'000'000});
    CommandSequence sequence = 1;
    for (std::size_t index = 0; index < kOrdersPerBatch; ++index) {
      const Price price = static_cast<Price>(100'000U + (index % 5'000U));
      static_cast<void>(
          book.process(Command{.sequence = sequence++,
                               .body = LimitGtc{.order_id = static_cast<OrderId>(index + 1U),
                                                .side = Side::Sell,
                                                .price = price,
                                                .quantity = 1U}}));
    }
    for (std::size_t index = 0; index < kOrdersPerBatch; ++index) {
      const Price price = static_cast<Price>(100'000U + (index % 5'000U));
      const auto events = book.process(
          Command{.sequence = sequence++,
                  .body = LimitIoc{.order_id = static_cast<OrderId>(kOrdersPerBatch + index + 1U),
                                   .side = Side::Buy,
                                   .price = price,
                                   .quantity = 1U}});
      benchmark::DoNotOptimize(events.data());
    }
    benchmark::DoNotOptimize(book.live_order_count());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(2U * kOrdersPerBatch));
  state.SetLabel("rest then cross with IOC");
}
BENCHMARK(BM_ReferenceBookCrossingIoc)->Unit(benchmark::kMillisecond);

void BM_ReferenceBookPartialAndCancel(benchmark::State& state) {
  constexpr std::size_t kOrdersPerBatch = 1'000;
  for (auto _ : state) {
    ReferenceBook book(BookConfig{.price_max = 1'000'000, .quantity_max = 1'000'000});
    CommandSequence sequence = 1;
    for (std::size_t index = 0; index < kOrdersPerBatch; ++index) {
      const auto events =
          book.process(Command{.sequence = sequence++,
                               .body = LimitGtc{.order_id = static_cast<OrderId>(index + 1U),
                                                .side = Side::Buy,
                                                .price = static_cast<Price>(10'000U + index),
                                                .quantity = 10U}});
      benchmark::DoNotOptimize(events.data());
    }
    for (std::size_t index = 0; index < kOrdersPerBatch / 2U; ++index) {
      const auto events = book.process(
          Command{.sequence = sequence++,
                  .body = ReplaceOrder{.order_id = static_cast<OrderId>(index + 1U),
                                       .new_price = static_cast<Price>(10'000U + index),
                                       .new_quantity = 5U}});
      benchmark::DoNotOptimize(events.data());
    }
    for (std::size_t index = kOrdersPerBatch / 2U; index < kOrdersPerBatch; ++index) {
      const auto events = book.process(
          Command{.sequence = sequence++, .body = CancelOrder{.order_id = index + 1U}});
      benchmark::DoNotOptimize(events.data());
    }
    benchmark::DoNotOptimize(book.live_order_count());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kOrdersPerBatch * 2U));
  state.SetLabel("GTC adds, quantity decreases, cancels");
}
BENCHMARK(BM_ReferenceBookPartialAndCancel)->Unit(benchmark::kMillisecond);

void BM_ReferenceBookAllocationProbe(benchmark::State& state) {
  constexpr std::size_t kOrdersPerBatch = 1'000;
  for (auto _ : state) {
    ReferenceBook book;
    CommandSequence sequence = 1;
    const std::uint64_t allocations_before = allocation_count_snapshot();
    for (std::size_t index = 0; index < kOrdersPerBatch; ++index) {
      static_cast<void>(
          book.process(Command{.sequence = sequence++,
                               .body = LimitGtc{.order_id = static_cast<OrderId>(index + 1U),
                                                .side = Side::Buy,
                                                .price = static_cast<Price>(20'000U + index),
                                                .quantity = 1U}}));
    }
    const std::uint64_t allocations_after = allocation_count_snapshot();
    state.counters["allocations/op"] = static_cast<double>(allocations_after - allocations_before) /
                                       static_cast<double>(kOrdersPerBatch);
    benchmark::DoNotOptimize(book.live_order_count());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kOrdersPerBatch));
  state.SetLabel("global new calls per resting add; includes output events");
}
BENCHMARK(BM_ReferenceBookAllocationProbe)->Iterations(10)->Unit(benchmark::kMillisecond);

void BM_MatchingPipelineBalancedAdds(benchmark::State& state) {
  constexpr std::size_t kCommandsPerBatch = 4'096;
  const auto bodies = make_balanced_workload(kCommandsPerBatch);
  std::vector<Command> workload;
  workload.reserve(bodies.size());
  CommandSequence sequence = 1;
  for (const auto& body : bodies) workload.push_back(Command{sequence++, body});

  std::uint64_t full_retries = 0;
  for (auto _ : state) {
    state.PauseTiming();
    MatchingPipeline<256> pipeline;
    state.ResumeTiming();

    std::size_t submitted = 0;
    std::size_t received = 0;
    while (received < workload.size()) {
      if (submitted < workload.size() && pipeline.try_submit(workload[submitted])) {
        ++submitted;
      } else if (submitted < workload.size()) {
        ++full_retries;
      }

      if (pipeline.try_receive()) {
        ++received;
      } else {
        std::this_thread::yield();
      }
    }
    benchmark::DoNotOptimize(received);
    benchmark::ClobberMemory();
    state.PauseTiming();
    static_cast<void>(pipeline.drain_and_stop());
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(workload.size()));
  state.counters["queue_full_retries/command"] =
      static_cast<double>(full_retries) /
      (static_cast<double>(state.iterations()) * static_cast<double>(workload.size()));
  state.SetLabel("SPSC pipeline, balanced non-crossing GTC adds; thread startup excluded");
}
BENCHMARK(BM_MatchingPipelineBalancedAdds)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace chronex
