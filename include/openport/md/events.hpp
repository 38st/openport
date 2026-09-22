#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "openport/md/contract.hpp"
#include "openport/md/time.hpp"

/// The normalised market-data vocabulary every provider adapter speaks. Nothing
/// downstream of an adapter knows which provider produced an event.
namespace openport::md {

/// Dense per-feed contract id, assigned by the adapter. A contract's definition is
/// always published before any event that refers to it.
using InstrumentId = std::uint32_t;

struct ContractDefinition {
  InstrumentId id = 0;
  OptionContract contract;
};

/// Best bid and offer. Sizes are in contracts; a zero price means no quote on that side.
struct OptionQuote {
  InstrumentId id = 0;
  Timestamp ts = 0;
  double bid = 0.0;
  double ask = 0.0;
  double bid_size = 0.0;
  double ask_size = 0.0;
};

struct OptionTrade {
  InstrumentId id = 0;
  Timestamp ts = 0;
  double price = 0.0;
  double size = 0.0;
};

struct OpenInterest {
  InstrumentId id = 0;
  Timestamp ts = 0;
  double contracts = 0.0;
};

/// Implied volatility and Greeks as published by the provider, converted to one set
/// of units: iv as a decimal (0.18), gamma per $1 of underlying, vega per vol point,
/// theta per calendar day, rho per 1% of rate, all per unit of underlying (not per
/// contract). Kept only for comparison with OpenPort's own numbers.
struct VendorGreeks {
  InstrumentId id = 0;
  Timestamp ts = 0;
  double iv = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0;
  double rho = 0.0;
};

struct UnderlyingQuote {
  std::string symbol;
  Timestamp ts = 0;
  double bid = 0.0;
  double ask = 0.0;
  double last = 0.0;
};

enum class FeedState : std::uint8_t { Connecting, Live, Delayed, Stale, Error, Stopped };

[[nodiscard]] constexpr std::string_view to_string(FeedState state) noexcept {
  switch (state) {
    case FeedState::Connecting:
      return "connecting";
    case FeedState::Live:
      return "live";
    case FeedState::Delayed:
      return "delayed";
    case FeedState::Stale:
      return "stale";
    case FeedState::Error:
      return "error";
    case FeedState::Stopped:
      return "stopped";
  }
  return "unknown";
}

struct ProviderStatus {
  Timestamp ts = 0;
  FeedState state = FeedState::Connecting;
  std::string message;
};

using Event = std::variant<ContractDefinition, OptionQuote, OptionTrade, OpenInterest,
                           VendorGreeks, UnderlyingQuote, ProviderStatus>;

}  // namespace openport::md
