#pragma once

#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

#include "chronex/journal/command_journal.hpp"
#include "chronex/pipeline/matching_pipeline.hpp"

namespace chronex {

// Write-ahead wrapper: a command is flushed to the journal before it enters the
// in-memory queue. Queue-full returns false without journaling the command.
template <std::size_t QueueCapacity>
class JournaledMatchingPipeline {
 public:
  explicit JournaledMatchingPipeline(const std::filesystem::path& journal_path,
                                     BookConfig config = {})
      : journal_(journal_path), pipeline_(config, [path = journal_path](ReferenceBook& book) {
          return CommandJournal::replay_into(path, book);
        }) {}

  [[nodiscard]] bool try_submit(const Command& command) {
    if (!pipeline_.has_capacity()) return false;
    journal_.append(command);
    if (!pipeline_.try_submit(command))
      throw std::logic_error("pipeline capacity changed during single-producer submit");
    return true;
  }

  [[nodiscard]] std::optional<PipelineResult> try_receive() noexcept {
    return pipeline_.try_receive();
  }

  [[nodiscard]] std::vector<PipelineResult> drain_and_stop() {
    return pipeline_.drain_and_stop();
  }

  // Stops intake, drains accepted commands, persists the final book snapshot,
  // and compacts the journal. Construct a new pipeline to resume afterward.
  [[nodiscard]] std::vector<PipelineResult> checkpoint_and_compact() {
    auto remaining = pipeline_.drain_and_stop();
    const auto snapshot = pipeline_.snapshot_after_stop();
    const auto path = journal_.path();
    journal_.close();
    CommandJournal::checkpoint_and_compact(path, snapshot);
    return remaining;
  }

 private:
  CommandJournal journal_;
  MatchingPipeline<QueueCapacity> pipeline_;
};

}  // namespace chronex
