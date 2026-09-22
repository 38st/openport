#include "openport/md/time.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>

namespace openport::md {
namespace {

/// Day of the month of the n-th Sunday (n >= 1) of a month.
[[nodiscard]] int nth_sunday(int year, int month, int n) noexcept {
  const int first = weekday(Date{year, month, 1});
  return 1 + (7 - first) % 7 + 7 * (n - 1);
}

[[nodiscard]] bool parse_int(std::string_view text, std::size_t pos, std::size_t len,
                             int& out) noexcept {
  if (pos + len > text.size()) return false;
  const char* begin = text.data() + pos;
  const auto [ptr, ec] = std::from_chars(begin, begin + len, out);
  return ec == std::errc() && ptr == begin + len;
}

}  // namespace

std::int64_t days_since_epoch(Date date) noexcept {
  const std::chrono::year_month_day ymd{std::chrono::year{date.year},
                                        std::chrono::month{static_cast<unsigned>(date.month)},
                                        std::chrono::day{static_cast<unsigned>(date.day)}};
  return std::chrono::sys_days{ymd}.time_since_epoch().count();
}

Date date_from_days(std::int64_t days) noexcept {
  const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{days}}};
  return {static_cast<int>(ymd.year()), static_cast<int>(static_cast<unsigned>(ymd.month())),
          static_cast<int>(static_cast<unsigned>(ymd.day()))};
}

int weekday(Date date) noexcept {
  const std::chrono::weekday wd{std::chrono::sys_days{std::chrono::days{days_since_epoch(date)}}};
  return static_cast<int>(wd.c_encoding());
}

int new_york_utc_offset_hours(Date date, int hour) noexcept {
  const Date dst_start{date.year, 3, nth_sunday(date.year, 3, 2)};
  const Date dst_end{date.year, 11, nth_sunday(date.year, 11, 1)};
  const bool after_start = date > dst_start || (date == dst_start && hour >= 2);
  const bool before_end = date < dst_end || (date == dst_end && hour < 2);
  return after_start && before_end ? -4 : -5;
}

Timestamp new_york_to_utc(Date date, int hour, int minute, int second) noexcept {
  const std::int64_t local_seconds =
      days_since_epoch(date) * 86'400 + hour * 3'600 + minute * 60 + second;
  const std::int64_t utc_seconds =
      local_seconds - new_york_utc_offset_hours(date, hour) * 3'600LL;
  return utc_seconds * kNanosPerSecond;
}

Timestamp now() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::optional<Timestamp> parse_datetime(std::string_view text, Zone zone) noexcept {
  // YYYY-MM-DD[ T]HH:MM:SS, optionally followed by a fraction or suffix we ignore.
  Date date;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (text.size() < 19 || text[4] != '-' || text[7] != '-' || (text[10] != ' ' && text[10] != 'T') ||
      text[13] != ':' || text[16] != ':') {
    return std::nullopt;
  }
  if (!parse_int(text, 0, 4, date.year) || !parse_int(text, 5, 2, date.month) ||
      !parse_int(text, 8, 2, date.day) || !parse_int(text, 11, 2, hour) ||
      !parse_int(text, 14, 2, minute) || !parse_int(text, 17, 2, second)) {
    return std::nullopt;
  }
  if (date.month < 1 || date.month > 12 || date.day < 1 || date.day > 31 || hour > 23 ||
      minute > 59 || second > 60) {
    return std::nullopt;
  }
  if (zone == Zone::NewYork) return new_york_to_utc(date, hour, minute, second);
  return (days_since_epoch(date) * 86'400 + hour * 3'600 + minute * 60 + second) * kNanosPerSecond;
}

std::string format_date(Date date) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d", date.year, date.month, date.day);
  return buffer;
}

std::string format_timestamp(Timestamp ts) {
  std::int64_t days = ts / kNanosPerDay;
  std::int64_t rem = ts % kNanosPerDay;
  if (rem < 0) {
    rem += kNanosPerDay;
    --days;
  }
  const Date date = date_from_days(days);
  const std::int64_t millis = rem / 1'000'000;
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02lld:%02lld:%02lld.%03lldZ", date.year,
                date.month, date.day, static_cast<long long>(millis / 3'600'000),
                static_cast<long long>(millis / 60'000 % 60),
                static_cast<long long>(millis / 1'000 % 60), static_cast<long long>(millis % 1'000));
  return buffer;
}

}  // namespace openport::md
