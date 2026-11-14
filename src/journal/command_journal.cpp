#include "chronex/journal/command_journal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <functional>
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
constexpr std::array<std::uint8_t, 4> kCheckpointMagic{'C', 'X', 'S', '1'};
constexpr std::size_t kHeaderSize = 12U;
constexpr std::uint32_t kMaxPayloadSize = 128U;
constexpr std::uintmax_t kMaxCheckpointPayloadSize = 1U << 30U;

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

void update_crc(std::uint32_t& crc, const std::uint8_t byte) noexcept {
  crc ^= byte;
  for (unsigned bit = 0; bit < 8U; ++bit) {
    const auto mask = static_cast<std::uint32_t>(0U - (crc & 1U));
    crc = (crc >> 1U) ^ (0xedb88320U & mask);
  }
}

template <typename Integer>
void update_crc_integer(std::uint32_t& crc, const Integer value) noexcept {
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto encoded = static_cast<Unsigned>(value);
  for (std::size_t i = 0; i < sizeof(Integer); ++i) {
    update_crc(crc, static_cast<std::uint8_t>(encoded >> (i * 8U)));
  }
}

[[nodiscard]] std::uint32_t checksum(const Bytes& bytes, const std::size_t offset = 0U) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = offset; i < bytes.size(); ++i) update_crc(crc, bytes[i]);
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

[[nodiscard]] std::filesystem::path checkpoint_path(const std::filesystem::path& journal_path) {
  auto result = journal_path;
  result += ".checkpoint";
  return result;
}

void validate_checkpoint_snapshot(const BookSnapshot& snapshot) {
  if (snapshot.config.price_max < 1 || snapshot.config.quantity_max == 0U)
    fail("checkpoint has invalid book limits");
  if (snapshot.next_acceptance_sequence == 0U ||
      (snapshot.last_command_sequence == 0U && !snapshot.orders.empty()) ||
      (snapshot.last_command_sequence != std::numeric_limits<CommandSequence>::max() &&
       snapshot.next_acceptance_sequence > snapshot.last_command_sequence + 1U))
    fail("checkpoint sequence metadata is inconsistent");

  std::vector<OrderId> order_ids;
  std::vector<AcceptanceSequence> acceptance_sequences;
  order_ids.reserve(snapshot.orders.size());
  acceptance_sequences.reserve(snapshot.orders.size());
  std::optional<Price> best_bid;
  std::optional<Price> best_ask;
  for (const auto& order : snapshot.orders) {
    if ((order.side != Side::Buy && order.side != Side::Sell) || order.price < 1 ||
        order.price > snapshot.config.price_max || order.remaining_quantity == 0U ||
        order.remaining_quantity > snapshot.config.quantity_max ||
        order.acceptance_sequence == 0U ||
        order.acceptance_sequence >= snapshot.next_acceptance_sequence)
      fail("checkpoint contains invalid resting order state");
    order_ids.push_back(order.order_id);
    acceptance_sequences.push_back(order.acceptance_sequence);
    if (order.side == Side::Buy) {
      best_bid = best_bid.has_value() ? std::max(*best_bid, order.price) : order.price;
    } else {
      best_ask = best_ask.has_value() ? std::min(*best_ask, order.price) : order.price;
    }
  }
  std::sort(order_ids.begin(), order_ids.end());
  std::sort(acceptance_sequences.begin(), acceptance_sequences.end());
  if (std::adjacent_find(order_ids.begin(), order_ids.end()) != order_ids.end() ||
      std::adjacent_find(acceptance_sequences.begin(), acceptance_sequences.end()) !=
          acceptance_sequences.end())
    fail("checkpoint contains duplicate order or acceptance sequences");
  if (best_bid.has_value() && best_ask.has_value() && *best_bid >= *best_ask)
    fail("checkpoint book is locked or crossed");
}

void write_checkpoint_frame(NativeAppendFile& output, const BookSnapshot& snapshot) {
  constexpr std::uintmax_t kOrderSize = 25U;
  constexpr std::uintmax_t kFixedSize = 32U;
  constexpr std::size_t kWriteChunkSize = 64U * 1024U;
  const auto count = static_cast<std::uintmax_t>(snapshot.orders.size());
  if (count > (kMaxCheckpointPayloadSize - kFixedSize) / kOrderSize)
    fail("checkpoint contains too many resting orders");

  const auto payload_size = static_cast<std::uint32_t>(kFixedSize + count * kOrderSize);
  std::uint32_t crc = 0xffffffffU;
  update_crc_integer(crc, snapshot.last_command_sequence);
  update_crc_integer(crc, snapshot.next_acceptance_sequence);
  update_crc_integer(crc, snapshot.config.price_max);
  update_crc_integer(crc, snapshot.config.quantity_max);
  update_crc_integer(crc, static_cast<std::uint64_t>(snapshot.orders.size()));
  for (const auto& order : snapshot.orders) {
    update_crc_integer(crc, order.order_id);
    update_crc_integer(crc, static_cast<std::uint8_t>(order.side));
    update_crc_integer(crc, order.price);
    update_crc_integer(crc, order.remaining_quantity);
    update_crc_integer(crc, order.acceptance_sequence);
  }

  Bytes header(kCheckpointMagic.begin(), kCheckpointMagic.end());
  encode_integer(header, payload_size);
  encode_integer(header, ~crc);
  output.write(header.data(), header.size());

  Bytes chunk;
  chunk.reserve(kWriteChunkSize + kOrderSize);
  const auto flush_if_full = [&output, &chunk] {
    if (chunk.size() >= kWriteChunkSize) {
      output.write(chunk.data(), chunk.size());
      chunk.clear();
    }
  };
  encode_integer(chunk, snapshot.last_command_sequence);
  encode_integer(chunk, snapshot.next_acceptance_sequence);
  encode_integer(chunk, snapshot.config.price_max);
  encode_integer(chunk, snapshot.config.quantity_max);
  encode_integer(chunk, static_cast<std::uint64_t>(snapshot.orders.size()));
  flush_if_full();
  for (const auto& order : snapshot.orders) {
    encode_integer(chunk, order.order_id);
    encode_integer(chunk, static_cast<std::uint8_t>(order.side));
    encode_integer(chunk, order.price);
    encode_integer(chunk, order.remaining_quantity);
    encode_integer(chunk, order.acceptance_sequence);
    flush_if_full();
  }
  if (!chunk.empty()) output.write(chunk.data(), chunk.size());
}

[[nodiscard]] std::optional<BookSnapshot> read_checkpoint(
    const std::filesystem::path& journal_path) {
  const auto path = checkpoint_path(journal_path);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (!std::filesystem::exists(path)) return std::nullopt;
    fail("cannot open checkpoint for reading");
  }
  const auto checkpoint_size = std::filesystem::file_size(path);
  if (checkpoint_size > kMaxCheckpointPayloadSize + kHeaderSize)
    fail("checkpoint file exceeds the configured size limit");
  Bytes bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) fail("failed while reading checkpoint");
  if (bytes.size() < kHeaderSize) fail("checkpoint frame is incomplete");
  for (std::size_t i = 0; i < kCheckpointMagic.size(); ++i) {
    if (bytes[i] != kCheckpointMagic[i]) fail("bad checkpoint magic or unsupported version");
  }

  std::size_t offset = 4U;
  const auto payload_size = decode_integer<std::uint32_t>(bytes, offset);
  const auto expected_checksum = decode_integer<std::uint32_t>(bytes, offset);
  if (payload_size > kMaxCheckpointPayloadSize || bytes.size() - kHeaderSize != payload_size)
    fail("checkpoint payload size is invalid");
  bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
  if (checksum(bytes) != expected_checksum) fail("checkpoint checksum mismatch");

  offset = 0U;
  BookSnapshot snapshot;
  snapshot.last_command_sequence = decode_integer<CommandSequence>(bytes, offset);
  snapshot.next_acceptance_sequence = decode_integer<AcceptanceSequence>(bytes, offset);
  snapshot.config.price_max = decode_integer<Price>(bytes, offset);
  snapshot.config.quantity_max = decode_integer<Quantity>(bytes, offset);
  const auto order_count = decode_integer<std::uint64_t>(bytes, offset);
  constexpr std::size_t kEncodedOrderSize = 25U;
  if (order_count > (bytes.size() - offset) / kEncodedOrderSize ||
      offset + static_cast<std::size_t>(order_count) * kEncodedOrderSize != bytes.size())
    fail("checkpoint order count is invalid");
  snapshot.orders.reserve(static_cast<std::size_t>(order_count));
  for (std::uint64_t i = 0; i < order_count; ++i) {
    const auto id = decode_integer<OrderId>(bytes, offset);
    const auto side = decode_integer<std::uint8_t>(bytes, offset);
    snapshot.orders.push_back(
        RestingOrder{.order_id = id,
                     .side = static_cast<Side>(side),
                     .price = decode_integer<Price>(bytes, offset),
                     .remaining_quantity = decode_integer<Quantity>(bytes, offset),
                     .acceptance_sequence = decode_integer<AcceptanceSequence>(bytes, offset)});
  }
  return snapshot;
}

struct ScanResult {
  std::uintmax_t valid_bytes{};
  CommandSequence last_sequence{};
};

[[nodiscard]] ScanResult scan(const std::filesystem::path& path,
                              const std::function<void(const Command&)>& visit = {},
                              const CommandSequence alternate_start_sequence = 0U) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (!std::filesystem::exists(path)) return {};
    fail("cannot open file for reading");
  }

  ScanResult result;
  CommandSequence expected_sequence = 1U;
  bool first_command = true;
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
    input.read(reinterpret_cast<char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
    const auto payload_bytes = static_cast<std::size_t>(input.gcount());
    if (input.bad() || (input.fail() && !input.eof())) fail("failed while reading frame payload");
    if (payload_bytes < payload_size) break;

    if (checksum(payload) != expected_checksum) fail("checksum mismatch");
    const auto command = decode_command(payload);
    if (first_command) {
      if (command.sequence != 1U && command.sequence != alternate_start_sequence)
        fail("first command sequence does not match the journal or checkpoint");
      expected_sequence = command.sequence;
      first_command = false;
    }
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
    const auto checkpoint = read_checkpoint(this->file_path);
    const auto checkpoint_sequence =
        checkpoint.has_value() ? checkpoint->last_command_sequence : 0U;
    const auto alternate_start = checkpoint_sequence == std::numeric_limits<CommandSequence>::max()
                                     ? 0U
                                     : checkpoint_sequence + 1U;
    const auto existing = scan(this->file_path, {}, alternate_start);
    const auto file_size =
        std::filesystem::exists(this->file_path) ? std::filesystem::file_size(this->file_path) : 0U;
    if (existing.valid_bytes < file_size)
      std::filesystem::resize_file(this->file_path, existing.valid_bytes);
    last_sequence = std::max(checkpoint_sequence, existing.last_sequence);
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
  bool closed{};
};

CommandJournal::CommandJournal(std::filesystem::path path, const JournalSyncPolicy sync_policy)
    : impl_(std::make_unique<Impl>(std::move(path), sync_policy)) {}
CommandJournal::~CommandJournal() = default;
CommandJournal::CommandJournal(CommandJournal&&) noexcept = default;
CommandJournal& CommandJournal::operator=(CommandJournal&&) noexcept = default;

void CommandJournal::append(const Command& command) {
  if (impl_->closed) fail("writer is closed");
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

void CommandJournal::close() noexcept {
  impl_->output.reset();
  impl_->closed = true;
}

const std::filesystem::path& CommandJournal::path() const noexcept {
  return impl_->file_path;
}

std::vector<Command> CommandJournal::recover(const std::filesystem::path& path) {
  const auto checkpoint = read_checkpoint(path);
  const CommandSequence checkpoint_sequence =
      checkpoint.has_value() ? checkpoint->last_command_sequence : 0U;
  const auto alternate_start = checkpoint_sequence == std::numeric_limits<CommandSequence>::max()
                                   ? 0U
                                   : checkpoint_sequence + 1U;
  std::vector<Command> commands;
  static_cast<void>(scan(
      path,
      [&commands, checkpoint_sequence](const Command& command) {
        if (command.sequence > checkpoint_sequence) commands.push_back(command);
      },
      alternate_start));
  return commands;
}

CommandSequence CommandJournal::replay_into(const std::filesystem::path& path,
                                            ReferenceBook& book) {
  const auto checkpoint = read_checkpoint(path);
  const CommandSequence checkpoint_sequence =
      checkpoint.has_value() ? checkpoint->last_command_sequence : 0U;
  if (checkpoint.has_value()) book.restore(*checkpoint);
  const auto alternate_start = checkpoint_sequence == std::numeric_limits<CommandSequence>::max()
                                   ? 0U
                                   : checkpoint_sequence + 1U;
  const auto journal = scan(
      path,
      [&book, checkpoint_sequence](const Command& command) {
        if (command.sequence > checkpoint_sequence) static_cast<void>(book.process(command));
      },
      alternate_start);
  return std::max(checkpoint_sequence, journal.last_sequence);
}

void CommandJournal::checkpoint_and_compact(const std::filesystem::path& path,
                                            const BookSnapshot& snapshot) {
  validate_checkpoint_snapshot(snapshot);
  if (!std::filesystem::exists(path)) fail("journal must exist before checkpointing");

  const auto previous_checkpoint = read_checkpoint(path);
  const CommandSequence checkpoint_sequence =
      previous_checkpoint.has_value() ? previous_checkpoint->last_command_sequence : 0U;
  const auto alternate_start = checkpoint_sequence == std::numeric_limits<CommandSequence>::max()
                                   ? 0U
                                   : checkpoint_sequence + 1U;
  const auto journal = scan(path, {}, alternate_start);
  const auto latest_sequence = std::max(checkpoint_sequence, journal.last_sequence);
  if (snapshot.last_command_sequence != latest_sequence)
    fail("checkpoint sequence does not match the stopped journal");

  const auto checkpoint = checkpoint_path(path);
  auto temporary = checkpoint;
  temporary += ".tmp";
  std::error_code remove_error;
  std::filesystem::remove(temporary, remove_error);
  if (remove_error)
    throw std::filesystem::filesystem_error("remove stale checkpoint temp", temporary,
                                            remove_error);

  {
    NativeAppendFile output(temporary);
    write_checkpoint_frame(output, snapshot);
    output.sync();
  }

#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), checkpoint.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    fail_system("MoveFileExW checkpoint");
#else
  if (::rename(temporary.c_str(), checkpoint.c_str()) != 0) fail_system("rename checkpoint");
  NativeAppendFile::sync_parent_directory(checkpoint);
#endif

#if defined(_WIN32)
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) fail_system("open journal for compaction");
  LARGE_INTEGER zero{};
  const bool seek_succeeded = SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) != 0;
  const bool truncate_succeeded = seek_succeeded && SetEndOfFile(handle) != 0;
  const bool sync_succeeded = truncate_succeeded && FlushFileBuffers(handle) != 0;
  const DWORD operation_error = GetLastError();
  CloseHandle(handle);
  if (!sync_succeeded)
    throw std::system_error(static_cast<int>(operation_error), std::system_category(),
                            "compact journal");
#else
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
  if (descriptor < 0) fail_system("open journal for compaction");
  int sync_result;
  do {
    sync_result = ::fsync(descriptor);
  } while (sync_result != 0 && errno == EINTR);
  const int sync_error = errno;
  ::close(descriptor);
  if (sync_result != 0)
    throw std::system_error(sync_error, std::generic_category(), "fsync compacted journal");
#endif
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
  const auto checkpoint = read_checkpoint(path);
  const CommandSequence checkpoint_sequence =
      checkpoint.has_value() ? checkpoint->last_command_sequence : 0U;
  if (checkpoint.has_value()) book.restore(*checkpoint);
  const auto alternate_start = checkpoint_sequence == std::numeric_limits<CommandSequence>::max()
                                   ? 0U
                                   : checkpoint_sequence + 1U;
  static_cast<void>(scan(
      path,
      [&book, &result, checkpoint_sequence](const Command& command) {
        if (command.sequence > checkpoint_sequence)
          result.command_events.push_back(book.process(command));
      },
      alternate_start));
  result.bids = book.resting_orders(Side::Buy);
  result.asks = book.resting_orders(Side::Sell);
  return result;
}

}  // namespace chronex
