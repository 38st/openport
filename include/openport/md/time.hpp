#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace openport::md {

/// Nanoseconds since the Unix epoch, UTC.
using Timestamp = std::int64_t;
inline constexpr Timestamp kInvalidTimestamp = std::numeric_limits<Timestamp>::min();

inline constexpr Timestamp kNanosPerSecond = 1'000'000'000;
inline constexpr Timestamp kNanosPerMinute = 60 * kNanosPerSecond;
inline constexpr Timestamp kNanosPerDay = 86'400 * kNanosPerSecond;
inline constexpr double kNanosPerYear = 365.0 * 86'400.0 * 1e9;

/// A calendar date in the proleptic Gregorian calendar.
struct Date {
  int year = 1970;
  int month = 1;
  int day = 1;

  friend constexpr auto operator<=>(const Date&, const Date&) = default;
};

[[nodiscard]] bool valid_date(Date date) noexcept;

[[nodiscard]] std::int64_t days_since_epoch(Date date) noexcept;
[[nodiscard]] Date date_from_days(std::int64_t days) noexcept;

/// Day of the week, 0 = Sunday.
[[nodiscard]] int weekday(Date date) noexcept;

/// UTC offset of New York at a local wall-clock time, in hours: -4 while daylight
/// saving is in force (second Sunday of March, 02:00, to first Sunday of November,
/// 02:00), otherwise -5.
[[nodiscard]] int new_york_utc_offset_hours(Date date, int hour) noexcept;

/// Converts a New York wall-clock time to UTC. Invalid fields, the spring DST
/// gap, or overflow return kInvalidTimestamp. Ambiguous fall times use the first
/// (EDT) occurrence. Uses the post-2007 US DST rules.
[[nodiscard]] Timestamp new_york_to_utc(Date date, int hour, int minute, int second = 0) noexcept;

/// An instant as New York wall-clock time: its local date and the seconds since
/// local midnight.
struct NewYorkTime {
  Date date;
  int seconds = 0;
};
[[nodiscard]] NewYorkTime new_york_time(Timestamp ts) noexcept;

/// Year fraction between two instants on an ACT/365 basis.
[[nodiscard]] constexpr double years_between(Timestamp from, Timestamp to) noexcept {
  // Preserve nanosecond differences when subtraction fits; only intervals
  // spanning opposite int64 extremes need a floating-point subtraction.
  if ((from < 0 && to > std::numeric_limits<Timestamp>::max() + from) ||
      (from > 0 && to < std::numeric_limits<Timestamp>::min() + from))
    return (static_cast<double>(to) - static_cast<double>(from)) / kNanosPerYear;
  return static_cast<double>(to - from) / kNanosPerYear;
}

/// Current wall-clock time.
[[nodiscard]] Timestamp now() noexcept;

enum class Zone : std::uint8_t { Utc, NewYork };

/// Parses "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS", with an optional fraction
/// of a second (1-9 digits), as a wall-clock time in `zone`. Z or +/-hh:mm
/// overrides `zone`; all other suffixes, impossible dates/fields and nanosecond
/// overflow are rejected. Unzoned New York times in the spring-forward gap are
/// rejected; ambiguous fall-back times use the first (EDT) occurrence.
[[nodiscard]] std::optional<Timestamp> parse_datetime(std::string_view text, Zone zone) noexcept;

/// Regular US options session calendar: NYSE/Cboe holidays and 13:00 ET early
/// closes for 2025-2028. Outside that range only weekdays are considered; holiday
/// and early-close rules are unavailable. This models 09:30-16:00 ET regular
/// hours, excluding extended/product-specific sessions and unscheduled halts.
struct MarketSession {
  bool open = false;
  std::string note;
  std::optional<Timestamp> next_open;  ///< null while open, or beyond timestamp range
};
[[nodiscard]] MarketSession market_session(Timestamp ts);

/// Per-root option sessions, independent of the underlying stock/index clock.
/// GTH belongs to the following trading date; no GTH before a holiday/weekend.
/// Simplifications: no special holiday GTH or curb on early-close days; outside
/// 2025-2028 only weekdays are known. Expiry settlement times remain separate.
struct TradingSession {
  std::string name = "closed";  ///< regular, curb, global, closed
  bool open = false;
  std::string note;
  /// ts while open, otherwise the most recent session end <= ts. Invalid if no
  /// preceding session end is representable in the nanosecond timestamp range.
  Timestamp market_time = kInvalidTimestamp;
};
[[nodiscard]] TradingSession trading_session(std::string_view root, Timestamp ts);

/// Scheduled PM close hour (13 or 16). Outside 2025-2028 assumes 16:00 ET;
/// does not roll a contract date off holidays or weekends.
[[nodiscard]] int regular_close_hour(Date date) noexcept;

/// "YYYY-MM-DD".
[[nodiscard]] std::string format_date(Date date);

/// ISO 8601 in UTC with milliseconds, e.g. "2026-09-22T19:48:44.000Z".
[[nodiscard]] std::string format_timestamp(Timestamp ts);

}  // namespace openport::md
