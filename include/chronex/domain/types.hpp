#pragma once

#include <cstdint>

namespace chronex {

using CommandSequence = std::uint64_t;
using EventIndex = std::uint32_t;
using OrderId = std::uint64_t;
using AcceptanceSequence = std::uint64_t;
using Price = std::int32_t;
using Quantity = std::uint32_t;

enum class Side : std::uint8_t {
  Buy,
  Sell,
};

[[nodiscard]] constexpr Side opposite(const Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace chronex
