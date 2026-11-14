#include "chronex/journal/command_journal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace chronex {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 4> kMagic{'C', 'X', 'J', '1'};
constexpr std::size_t kHeaderSize = 12U;
constexpr std::uint32_t kMaxPayloadSize = 128U;

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error("journal: " + message);
}

[[noreturn]] void fail_system(const char* operation) {
#if defined(_WIN32)
  throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
#else
  throw std::system_error(errno, std::generic_category(), operation);
#endif
}

class NativeAppendFile {
 public:
  explicit NativeAppendFile(const std::filesystem::path& path) {
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_WRITE | FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) fail_system("CreateFileW");
#else
    descriptor_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor_ < 0) fail_system("open journal");
#endif
  }

  ~NativeAppendFile() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (descriptor_ >= 0) ::close(descriptor_);
#endif
  }

  NativeAppendFile(const NativeAppendFile&) = delete;
  NativeAppendFile& operator=(const NativeAppendFile&) = delete;

  void write(const std::uint8_t* data, std::size_t size) {
    while (size > 0U) {
#if defined(_WIN32)
      const auto chunk_size = static_cast<DWORD>(
          std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
      DWORD written = 0;
      if (!WriteFile(handle_, data, chunk_size, &written, nullptr)) fail_system("WriteFile");
      if (written == 0U) fail("WriteFile made no progress");
      const auto written_size = static_cast<std::size_t>(written);
#else
      const auto written = ::write(descriptor_, data, size);
      if (written < 0) {
        if (errno == EINTR) continue;
        fail_system("write journal");
      }
      if (written == 0) fail("write made no progress");
      const auto written_size = static_cast<std::size_t>(written);
#endif
      data += written_size;
      size -= written_size;
    }
  }

  void sync() {
#if defined(_WIN32)
    if (!FlushFileBuffers(handle_)) fail_system("FlushFileBuffers");
#else
    while (::fsync(descriptor_) != 0) {
      if (errno == EINTR) continue;
      fail_system("fsync journal");
    }
#endif
  }

#if !defined(_WIN32)
  static void sync_parent_directory(const std::filesystem::path& file_path) {
    auto directory = file_path.parent_path();
    if (directory.empty()) directory = ".";
    const int directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) fail_system("open journal directory");
    int result;
    do {
      result = ::fsync(directory_fd);
    } while (result != 0 && errno == EINTR);
    const int sync_error = errno;
    ::close(directory_fd);
    if (result != 0)
      throw std::system_error(sync_error, std::generic_category(), "fsync directory");
  }
#endif

 private:
#if defined(_WIN32)
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int descriptor_{-1};
#endif
};

template <typename Integer>
void encode_integer(Bytes& out, Integer value) {
  static_assert(std::is_integral_v<Integer>);
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto bits = static_cast<Unsigned>(value);
  for (std::size_t i = 0; i < sizeof(Integer); ++i) {
    out.push_back(static_cast<std::uint8_t>((bits >> (i * 8U)) & static_cast<Unsigned>(0xffU)));
  }
}

template <typename Integer>
Integer decode_integer(const Bytes& bytes, std::size_t& offset) {
  static_assert(std::is_integral_v<Integer>);
  if (bytes.size() - offset < sizeof(Integer)) fail("truncated command payload");
  using Unsigned = std::make_unsigned_t<Integer>;
  Unsigned value{};
  for (std::size_t i = 0; i < sizeof(Integer); ++i) {
    value |= static_cast<Unsigned>(bytes[offset++]) << (i * 8U);
  }
  if constexpr (std::is_signed_v<Integer>) {
    return std::bit_cast<Integer>(value);
  } else {
    return value;
  }
}

[[nodiscard]] std::uint32_t checksum(const Bytes& bytes) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (const auto byte : bytes) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8U; ++bit) {
      const auto mask = static_cast<std::uint32_t>(0U - (crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

[[nodiscard]] Bytes encode_command(const Command& command) {
  Bytes payload;
  payload.reserve(32U);
  encode_integer(payload, command.sequence);
  std::visit(
      [&payload](const auto& body) {
        using Body = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<Body, LimitGtc>) {
          encode_integer(payload, std::uint8_t{1});
          encode_integer(payload, body.order_id);
          encode_integer(payload, static_cast<std::uint8_t>(body.side));
          encode_integer(payload, body.price);
          encode_integer(payload, body.quantity);
        } else if constexpr (std::is_same_v<Body, LimitIoc>) {
          encode_integer(payload, std::uint8_t{2});
          encode_integer(payload, body.order_id);
          encode_integer(payload, static_cast<std::uint8_t>(body.side));
          encode_integer(payload, body.price);
          encode_integer(payload, body.quantity);
        } else if constexpr (std::is_same_v<Body, MarketOrder>) {
          encode_integer(payload, std::uint8_t{3});
          encode_integer(payload, body.order_id);
          encode_integer(payload, static_cast<std::uint8_t>(body.side));
          encode_integer(payload, body.quantity);
          encode_integer(payload, body.protection_ticks);
        } else if constexpr (std::is_same_v<Body, CancelOrder>) {
          encode_integer(payload, std::uint8_t{4});
          encode_integer(payload, body.order_id);
        } else {
          encode_integer(payload, std::uint8_t{5});
          encode_integer(payload, body.order_id);
          encode_integer(payload, body.new_price);
          encode_integer(payload, body.new_quantity);
        }
      },
      command.body);
  return payload;
}

[[nodiscard]] Command decode_command(const Bytes& payload) {
  std::size_t offset = 0U;
  const auto sequence = decode_integer<CommandSequence>(payload, offset);
  const auto tag = decode_integer<std::uint8_t>(payload, offset);
  CommandBody body;
  switch (tag) {
    case 1U:
    case 2U: {
      const auto id = decode_integer<OrderId>(payload, offset);
      const auto side_value = decode_integer<std::uint8_t>(payload, offset);
      if (side_value > 1U) fail("invalid side value");
      const auto side = static_cast<Side>(side_value);
      const auto price = decode_integer<Price>(payload, offset);
      const auto quantity = decode_integer<Quantity>(payload, offset);
      if (tag == 1U)
        body = LimitGtc{.order_id = id, .side = side, .price = price, .quantity = quantity};
      else
        body = LimitIoc{.order_id = id, .side = side, .price = price, .quantity = quantity};
      break;
    }
    case 3U: {
      const auto id = decode_integer<OrderId>(payload, offset);
      const auto side_value = decode_integer<std::uint8_t>(payload, offset);
      if (side_value > 1U) fail("invalid side value");
      body = MarketOrder{.order_id = id,
                         .side = static_cast<Side>(side_value),
                         .quantity = decode_integer<Quantity>(payload, offset),
                         .protection_ticks = decode_integer<Price>(payload, offset)};
      break;
    }
    case 4U:
      body = CancelOrder{.order_id = decode_integer<OrderId>(payload, offset)};
      break;
    case 5U:
      body = ReplaceOrder{.order_id = decode_integer<OrderId>(payload, offset),
                          .new_price = decode_integer<Price>(payload, offset),
                          .new_quantity = decode_integer<Quantity>(payload, offset)};
      break;
    default:
      fail("unknown command tag");
  }
  if (offset != payload.size()) fail("unexpected trailing command payload data");
  return Command{.sequence = sequence, .body = std::move(body)};
}

struct ScanResult {
  std::uintmax_t valid_bytes{};
  CommandSequence last_sequence{};
};

[[nodiscard]] ScanResult scan(const std::filesystem::path& path,
                              const std::function<void(const Command&)>& visit = {}) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (!std::filesystem::exists(path)) return {};
    fail("cannot open file for reading");
  }

  ScanResult result;
  CommandSequence expected_sequence = 1U;
  bool sequence_exhausted = false;
  for (;;) {
    std::array<std::uint8_t, kHeaderSize> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto header_bytes = static_cast<std::size_t>(input.gcount());
    if (header_bytes == 0U && input.eof()) break;
    if (input.bad() || (input.fail() && !input.eof())) fail("failed while reading frame header");
    if (sequence_exhausted) fail("command sequence exhausted");
    if (header_bytes < kHeaderSize) break;

    for (std::size_t i = 0; i < kMagic.size(); ++i) {
      if (header[i] != kMagic[i]) fail("bad frame magic or unsupported version");
    }
    Bytes encoded_header(header.begin(), header.end());
    std::size_t header_offset = 4U;
    const auto payload_size = decode_integer<std::uint32_t>(encoded_header, header_offset);
    const auto expected_checksum = decode_integer<std::uint32_t>(encoded_header, header_offset);
    if (payload_size > kMaxPayloadSize) fail("frame payload exceeds limit");
    Bytes payload(payload_size);
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    const auto payload_bytes = static_cast<std::size_t>(input.gcount());
    if (input.bad() || (input.fail() && !input.eof())) fail("failed while reading frame payload");
    if (payload_bytes < payload_size) break;

    if (checksum(payload) != expected_checksum) fail("checksum mismatch");
    const auto command = decode_command(payload);
    if (command.sequence != expected_sequence) fail("command sequence is not contiguous");
    if (visit) visit(command);
    result.last_sequence = command.sequence;
    result.valid_bytes += static_cast<std::uintmax_t>(kHeaderSize) + payload_size;
    if (expected_sequence == std::numeric_limits<CommandSequence>::max()) {
      sequence_exhausted = true;
    } else {
      ++expected_sequence;
    }
  }
  return result;
}

}  // namespace

struct CommandJournal::Impl {
  explicit Impl(std::filesystem::path file_path, const JournalSyncPolicy policy)
      : file_path(std::move(file_path)), sync_policy(policy) {
    if (sync_policy != JournalSyncPolicy::None && sync_policy != JournalSyncPolicy::EachAppend)
      fail("unknown synchronization policy");
    const auto parent_directory = this->file_path.parent_path();
    if (!parent_directory.empty() && !std::filesystem::exists(parent_directory)) {
      if (sync_policy == JournalSyncPolicy::EachAppend)
        fail("parent directory must exist when per-append sync is enabled");
      std::filesystem::create_directories(parent_directory);
    }
    const auto existing = scan(this->file_path);
    const auto file_size =
        std::filesystem::exists(this->file_path) ? std::filesystem::file_size(this->file_path) : 0U;
    if (existing.valid_bytes < file_size)
      std::filesystem::resize_file(this->file_path, existing.valid_bytes);
    last_sequence = existing.last_sequence;
    output = std::make_unique<NativeAppendFile>(this->file_path);
    if (sync_policy == JournalSyncPolicy::EachAppend) {
#if !defined(_WIN32)
      NativeAppendFile::sync_parent_directory(this->file_path);
#endif
    }
  }
  std::filesystem::path file_path;
  std::unique_ptr<NativeAppendFile> output;
  JournalSyncPolicy sync_policy;
  CommandSequence last_sequence{};
  bool poisoned{};
};

CommandJournal::CommandJournal(std::filesystem::path path, const JournalSyncPolicy sync_policy)
    : impl_(std::make_unique<Impl>(std::move(path), sync_policy)) {}
CommandJournal::~CommandJournal() = default;
CommandJournal::CommandJournal(CommandJournal&&) noexcept = default;
CommandJournal& CommandJournal::operator=(CommandJournal&&) noexcept = default;

void CommandJournal::append(const Command& command) {
  if (impl_->poisoned) fail("writer is poisoned after an earlier I/O failure; reopen for recovery");
  if (impl_->last_sequence == std::numeric_limits<CommandSequence>::max())
    fail("command sequence exhausted");
  if (command.sequence != impl_->last_sequence + 1U) fail("append sequence is not contiguous");
  const auto payload = encode_command(command);
  Bytes header(kMagic.begin(), kMagic.end());
  encode_integer(header, static_cast<std::uint32_t>(payload.size()));
  encode_integer(header, checksum(payload));
  Bytes frame;
  frame.reserve(header.size() + payload.size());
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), payload.begin(), payload.end());
  try {
    impl_->output->write(frame.data(), frame.size());
    if (impl_->sync_policy == JournalSyncPolicy::EachAppend) impl_->output->sync();
  } catch (...) {
    impl_->poisoned = true;
    throw;
  }
  impl_->last_sequence = command.sequence;
}

const std::filesystem::path& CommandJournal::path() const noexcept {
  return impl_->file_path;
}

std::vector<Command> CommandJournal::recover(const std::filesystem::path& path) {
  std::vector<Command> commands;
  static_cast<void>(scan(path, [&commands](const Command& command) { commands.push_back(command); }));
  return commands;
}

CommandSequence CommandJournal::replay_into(const std::filesystem::path& path,
                                            ReferenceBook& book) {
  return scan(path, [&book](const Command& command) { static_cast<void>(book.process(command)); })
      .last_sequence;
}

ReplayResult replay_commands(const std::vector<Command>& commands, const BookConfig config) {
  ReferenceBook book(config);
  ReplayResult result;
  result.command_events.reserve(commands.size());
  for (const auto& command : commands) result.command_events.push_back(book.process(command));
  result.bids = book.resting_orders(Side::Buy);
  result.asks = book.resting_orders(Side::Sell);
  return result;
}

ReplayResult replay_journal(const std::filesystem::path& path, const BookConfig config) {
  ReferenceBook book(config);
  ReplayResult result;
  static_cast<void>(scan(path, [&book, &result](const Command& command) {
    result.command_events.push_back(book.process(command));
  }));
  result.bids = book.resting_orders(Side::Buy);
  result.asks = book.resting_orders(Side::Sell);
  return result;
}

}  // namespace chronex
