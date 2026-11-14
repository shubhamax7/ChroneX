#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "chronex/oracle/reference_book.hpp"

namespace chronex {

enum class JournalSyncPolicy {
  None,
  EachAppend,
};

class CommandJournal {
 public:
  explicit CommandJournal(std::filesystem::path path,
                          JournalSyncPolicy sync_policy = JournalSyncPolicy::EachAppend);
  ~CommandJournal();
  CommandJournal(const CommandJournal&) = delete;
  CommandJournal& operator=(const CommandJournal&) = delete;
  CommandJournal(CommandJournal&&) noexcept;
  CommandJournal& operator=(CommandJournal&&) noexcept;

  // Appends one checksummed, versioned command. EachAppend requests an OS-level
  // file sync before returning; this cannot override filesystem/device limits.
  void append(const Command& command);
  void close() noexcept;
  [[nodiscard]] const std::filesystem::path& path() const noexcept;

  // Returns commands after the latest checkpoint, or the whole journal when no
  // checkpoint exists. A torn final frame is ignored and truncated when opening
  // for append; complete frames with invalid checksums or sequences are rejected.
  [[nodiscard]] static std::vector<Command> recover(const std::filesystem::path& path);

  // Replays complete frames directly into an empty book without materializing
  // the command history. Returns the last recovered command sequence (zero if
  // the journal is empty). If recovery throws, the book may contain a prefix
  // of the journal and should be discarded.
  [[nodiscard]] static CommandSequence replay_into(const std::filesystem::path& path,
                                                   ReferenceBook& book);

  // Atomically installs a checksummed checkpoint, then truncates the journal.
  // All writers must be closed and command intake stopped. The snapshot must be
  // the exact book state at the latest journal sequence.
  static void checkpoint_and_compact(const std::filesystem::path& path,
                                     const BookSnapshot& snapshot);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct ReplayResult {
  std::vector<std::vector<Event>> command_events;
  std::vector<RestingOrder> bids;
  std::vector<RestingOrder> asks;
};

[[nodiscard]] ReplayResult replay_commands(const std::vector<Command>& commands,
                                           BookConfig config = {});
[[nodiscard]] ReplayResult replay_journal(const std::filesystem::path& path,
                                          BookConfig config = {});

}  // namespace chronex
