#include "chronex/journal/command_journal.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace chronex {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 4> kMagic{'C', 'X', 'J', '1'};
constexpr std::size_t kHeaderSize = 12U;
constexpr std::uint32_t kMaxPayloadSize = 128U;

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error("journal: " + message);
}

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
  std::vector<Command> commands;
  std::uintmax_t valid_bytes{};
};

[[nodiscard]] ScanResult scan(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (!std::filesystem::exists(path)) return {};
    fail("cannot open file for reading");
  }
  Bytes bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) fail("failed while reading file");

  ScanResult result;
  std::size_t offset = 0U;
  CommandSequence expected_sequence = 1U;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < kHeaderSize) break;
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
      if (bytes[offset + i] != kMagic[i]) fail("bad frame magic or unsupported version");
    }
    std::size_t header_offset = offset + 4U;
    const auto payload_size = decode_integer<std::uint32_t>(bytes, header_offset);
    const auto expected_checksum = decode_integer<std::uint32_t>(bytes, header_offset);
    if (payload_size > kMaxPayloadSize) fail("frame payload exceeds limit");
    if (bytes.size() - header_offset < payload_size) break;
    Bytes payload(bytes.begin() + static_cast<std::ptrdiff_t>(header_offset),
                  bytes.begin() + static_cast<std::ptrdiff_t>(header_offset + payload_size));
    if (checksum(payload) != expected_checksum) fail("checksum mismatch");
    auto command = decode_command(payload);
    if (command.sequence != expected_sequence) fail("command sequence is not contiguous");
    result.commands.push_back(std::move(command));
    if (expected_sequence == std::numeric_limits<CommandSequence>::max() &&
        offset + kHeaderSize + payload_size < bytes.size())
      fail("command sequence exhausted");
    ++expected_sequence;
    offset = header_offset + payload_size;
    result.valid_bytes = static_cast<std::uintmax_t>(offset);
  }
  return result;
}

}  // namespace

struct CommandJournal::Impl {
  explicit Impl(std::filesystem::path file_path) : file_path(std::move(file_path)) {
    if (this->file_path.has_parent_path())
      std::filesystem::create_directories(this->file_path.parent_path());
    const auto existing = scan(this->file_path);
    const auto file_size =
        std::filesystem::exists(this->file_path) ? std::filesystem::file_size(this->file_path) : 0U;
    if (existing.valid_bytes < file_size)
      std::filesystem::resize_file(this->file_path, existing.valid_bytes);
    last_sequence = existing.commands.empty() ? 0U : existing.commands.back().sequence;
    output.open(this->file_path, std::ios::binary | std::ios::app);
    if (!output) fail("cannot open file for append");
  }
  std::filesystem::path file_path;
  std::ofstream output;
  CommandSequence last_sequence{};
};

CommandJournal::CommandJournal(std::filesystem::path path)
    : impl_(std::make_unique<Impl>(std::move(path))) {}
CommandJournal::~CommandJournal() = default;
CommandJournal::CommandJournal(CommandJournal&&) noexcept = default;
CommandJournal& CommandJournal::operator=(CommandJournal&&) noexcept = default;

void CommandJournal::append(const Command& command) {
  if (command.sequence != impl_->last_sequence + 1U) fail("append sequence is not contiguous");
  const auto payload = encode_command(command);
  Bytes header(kMagic.begin(), kMagic.end());
  encode_integer(header, static_cast<std::uint32_t>(payload.size()));
  encode_integer(header, checksum(payload));
  impl_->output.write(reinterpret_cast<const char*>(header.data()),
                      static_cast<std::streamsize>(header.size()));
  impl_->output.write(reinterpret_cast<const char*>(payload.data()),
                      static_cast<std::streamsize>(payload.size()));
  impl_->output.flush();
  if (!impl_->output) fail("write or stream flush failed");
  impl_->last_sequence = command.sequence;
}

const std::filesystem::path& CommandJournal::path() const noexcept {
  return impl_->file_path;
}

std::vector<Command> CommandJournal::recover(const std::filesystem::path& path) {
  return scan(path).commands;
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
  return replay_commands(CommandJournal::recover(path), config);
}

}  // namespace chronex
