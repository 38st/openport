#pragma once

#include <cstdint>
#include <string_view>

namespace openport::pricing {

enum class OptionType : std::uint8_t { Call, Put };

/// +1 for calls, -1 for puts: the "omega" that lets one formula price both.
[[nodiscard]] constexpr double omega(OptionType type) noexcept {
  return type == OptionType::Call ? 1.0 : -1.0;
}

[[nodiscard]] constexpr OptionType opposite(OptionType type) noexcept {
  return type == OptionType::Call ? OptionType::Put : OptionType::Call;
}

[[nodiscard]] constexpr std::string_view to_string(OptionType type) noexcept {
  return type == OptionType::Call ? "call" : "put";
}

}  // namespace openport::pricing
