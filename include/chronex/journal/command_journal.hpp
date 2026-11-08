#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "chronex/oracle/reference_book.hpp"

namespace chronex {

class CommandJournal {
 public:
  explicit CommandJournal(std::filesystem::path path);
  ~CommandJournal();
  CommandJournal(const CommandJournal&) = delete;
  CommandJournal& operator=(const CommandJournal&) = delete;
  CommandJournal(CommandJournal&&) noexcept;
  CommandJournal& operator=(CommandJournal&&) noexcept;

  // Appends one checksummed, versioned command and flushes the C++ file stream.
  // This is not an fsync/power-loss durability guarantee.
  void append(const Command& command);
  [[nodiscard]] const std::filesystem::path& path() const noexcept;

  // A torn final frame is ignored and truncated when opening for append. Any
  // complete frame with an invalid checksum, version, or sequence is rejected.
  [[nodiscard]] static std::vector<Command> recover(const std::filesystem::path& path);

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
