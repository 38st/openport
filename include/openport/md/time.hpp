#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace openport::md {

/// Nanoseconds since the Unix epoch, UTC.
using Timestamp = std::int64_t;

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

[[nodiscard]] std::int64_t days_since_epoch(Date date) noexcept;
[[nodiscard]] Date date_from_days(std::int64_t days) noexcept;

/// Day of the week, 0 = Sunday.
[[nodiscard]] int weekday(Date date) noexcept;

/// UTC offset of New York at a local wall-clock time, in hours: -4 while daylight
/// saving is in force (second Sunday of March, 02:00, to first Sunday of November,
/// 02:00), otherwise -5.
[[nodiscard]] int new_york_utc_offset_hours(Date date, int hour) noexcept;

/// Converts a New York wall-clock time to a UTC timestamp.
[[nodiscard]] Timestamp new_york_to_utc(Date date, int hour, int minute, int second = 0) noexcept;

/// Year fraction between two instants on an ACT/365 basis.
[[nodiscard]] constexpr double years_between(Timestamp from, Timestamp to) noexcept {
  return static_cast<double>(to - from) / kNanosPerYear;
}

/// Current wall-clock time.
[[nodiscard]] Timestamp now() noexcept;

enum class Zone : std::uint8_t { Utc, NewYork };

/// Parses "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS", with an optional fraction
/// of a second ("...:42.123"), as a wall-clock time in `zone`.
[[nodiscard]] std::optional<Timestamp> parse_datetime(std::string_view text, Zone zone) noexcept;

/// "YYYY-MM-DD".
[[nodiscard]] std::string format_date(Date date);

/// ISO 8601 in UTC with milliseconds, e.g. "2026-09-22T19:48:44.000Z".
[[nodiscard]] std::string format_timestamp(Timestamp ts);

}  // namespace openport::md
