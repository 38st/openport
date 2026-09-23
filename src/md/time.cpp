#include "openport/md/time.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <limits>

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
  for (std::size_t i = pos; i < pos + len; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  const char* begin = text.data() + pos;
  const auto [ptr, ec] = std::from_chars(begin, begin + len, out);
  return ec == std::errc() && ptr == begin + len;
}

// int128 keeps the final multiply/add checked even at the two int64 endpoints.
std::optional<Timestamp> timestamp(std::int64_t seconds, Timestamp fraction = 0) noexcept {
  const __int128 nanos = static_cast<__int128>(seconds) * kNanosPerSecond + fraction;
  if (nanos < std::numeric_limits<Timestamp>::min() ||
      nanos > std::numeric_limits<Timestamp>::max())
    return std::nullopt;
  return static_cast<Timestamp>(nanos);
}

bool valid_time(Date date, int hour, int minute, int second) noexcept {
  return valid_date(date) && hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 &&
         second < 60;
}

bool spring_gap(Date date, int hour) noexcept {
  return date.month == 3 && date.day == nth_sunday(date.year, 3, 2) && hour == 2;
}

// Published regular-session calendars (product-specific extended hours excluded):
// https://ir.theice.com/press/news-details/2024/NYSE-Group-Announces-2025-2026-and-2027-Holiday-and-Early-Closings-Calendar/default.aspx
// https://ir.theice.com/press/news-details/2025/NYSE-Group-Announces-2026-2027-and-2028-Holiday-and-Early-Closings-Calendar/
// https://www.cboe.com/about/hours/us-options
// Special closure:
// https://cdn.cboe.com/resources/schedule_update/2025/Update-Cboe-to-Observe-National-Day-of-Mourning-on-Thursday-January-9-2025.pdf
struct Holiday {
  std::string_view name;
  std::array<int, 4> month_day;  // 2025..2028, MMDD; zero means not observed
};
constexpr Holiday kHolidays[] = {
    {"New Year's Day", {101, 101, 101, 0}},
    {"National Day of Mourning", {109, 0, 0, 0}},
    {"Martin Luther King Jr. Day", {120, 119, 118, 117}},
    {"Washington's Birthday", {217, 216, 215, 221}},
    {"Good Friday", {418, 403, 326, 414}},
    {"Memorial Day", {526, 525, 531, 529}},
    {"Juneteenth", {619, 619, 618, 619}},
    {"Independence Day", {704, 703, 705, 704}},
    {"Labor Day", {901, 907, 906, 904}},
    {"Thanksgiving Day", {1127, 1126, 1125, 1123}},
    {"Christmas Day", {1225, 1225, 1224, 1225}},
};
constexpr Date kEarlyCloses[] = {{2025, 7, 3},   {2025, 11, 28}, {2025, 12, 24}, {2026, 11, 27},
                                 {2026, 12, 24}, {2027, 11, 26}, {2028, 7, 3},   {2028, 11, 24}};

std::string_view holiday(Date date) noexcept {
  if (date.year < 2025 || date.year > 2028) return {};
  for (const auto& h : kHolidays)
    if (h.month_day[date.year - 2025] == date.month * 100 + date.day) return h.name;
  return {};
}

bool business_day(Date date) noexcept {
  const int wd = weekday(date);
  return wd != 0 && wd != 6 && holiday(date).empty();
}

}  // namespace

bool valid_date(Date date) noexcept {
  if (date.year < 1 || date.year > 9999 || date.month < 1 || date.month > 12 || date.day < 1 ||
      date.day > 31)
    return false;
  return std::chrono::year_month_day{std::chrono::year{date.year},
                                     std::chrono::month{static_cast<unsigned>(date.month)},
                                     std::chrono::day{static_cast<unsigned>(date.day)}}
      .ok();
}

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
  if (!valid_time(date, hour, minute, second) || spring_gap(date, hour)) return kInvalidTimestamp;
  const std::int64_t local_seconds =
      days_since_epoch(date) * 86'400 + hour * 3'600 + minute * 60 + second;
  const std::int64_t utc_seconds = local_seconds - new_york_utc_offset_hours(date, hour) * 3'600LL;
  return timestamp(utc_seconds).value_or(kInvalidTimestamp);
}

Timestamp now() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::optional<Timestamp> parse_datetime(std::string_view text, Zone zone) noexcept {
  Date date;
  int hour = 0, minute = 0, second = 0;
  if (text.size() < 19 || text[4] != '-' || text[7] != '-' ||
      (text[10] != ' ' && text[10] != 'T') || text[13] != ':' || text[16] != ':')
    return std::nullopt;
  if (!parse_int(text, 0, 4, date.year) || !parse_int(text, 5, 2, date.month) ||
      !parse_int(text, 8, 2, date.day) || !parse_int(text, 11, 2, hour) ||
      !parse_int(text, 14, 2, minute) || !parse_int(text, 17, 2, second) ||
      !valid_time(date, hour, minute, second))
    return std::nullopt;

  std::size_t pos = 19;
  Timestamp fraction = 0;
  if (pos < text.size() && text[pos] == '.') {
    const auto first = ++pos;
    Timestamp scale = kNanosPerSecond;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (pos - first == 9) return std::nullopt;
      scale /= 10;
      fraction += (text[pos++] - '0') * scale;
    }
    if (pos == first) return std::nullopt;
  }
  int offset_minutes = 0;
  if (pos == text.size()) {
    if (zone == Zone::NewYork) {
      if (spring_gap(date, hour)) return std::nullopt;
      offset_minutes = 60 * new_york_utc_offset_hours(date, hour);
    }
  } else if (text.substr(pos) == "Z") {
    offset_minutes = 0;
  } else {
    int zh = 0, zm = 0;
    if (text.size() - pos != 6 || (text[pos] != '+' && text[pos] != '-') || text[pos + 3] != ':' ||
        !parse_int(text, pos + 1, 2, zh) || !parse_int(text, pos + 4, 2, zm) || zh > 23 || zm > 59)
      return std::nullopt;
    offset_minutes = (text[pos] == '+' ? 1 : -1) * (zh * 60 + zm);
  }
  const auto seconds =
      days_since_epoch(date) * 86400 + hour * 3600 + minute * 60 + second - offset_minutes * 60;
  return timestamp(seconds, fraction);
}

int regular_close_hour(Date date) noexcept {
  for (const auto early : kEarlyCloses)
    if (early == date) return 13;
  return 16;
}

MarketSession market_session(Timestamp ts) {
  // Determine DST from UTC transition instants, so the repeated fall hour cannot
  // choose the wrong local date. Work in seconds to avoid nanosecond overflow.
  auto seconds = ts / kNanosPerSecond;
  if (ts % kNanosPerSecond < 0) --seconds;
  auto utc_days = seconds / 86400;
  if (seconds % 86400 < 0) --utc_days;
  const auto utc_date = date_from_days(utc_days);
  const auto start =
      days_since_epoch({utc_date.year, 3, nth_sunday(utc_date.year, 3, 2)}) * 86400 + 7 * 3600;
  const auto end =
      days_since_epoch({utc_date.year, 11, nth_sunday(utc_date.year, 11, 1)}) * 86400 + 6 * 3600;
  const auto local = seconds + (seconds >= start && seconds < end ? -4 : -5) * 3600;
  auto days = local / 86400;
  auto rem = local % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  const auto date = date_from_days(days);
  MarketSession result;
  const int wd = weekday(date);
  if (wd == 0 || wd == 6)
    result.note = "closed (weekend)";
  else if (const auto h = holiday(date); !h.empty())
    result.note = "closed (holiday: " + std::string(h) + ")";
  else if (rem < 9 * 3600 + 30 * 60)
    result.note = "closed (pre-market)";
  else if (rem >= regular_close_hour(date) * 3600)
    result.note = "closed (after hours)";
  else {
    result.open = true;
    result.note = regular_close_hour(date) == 13 ? "open, early close 13:00 ET" : "open";
    return result;
  }
  for (int ahead = 0; ahead < 14; ++ahead) {
    const auto candidate = date_from_days(days + ahead);
    if (!business_day(candidate)) continue;
    const auto next = new_york_to_utc(candidate, 9, 30);
    if (next != kInvalidTimestamp && next > ts) {
      result.next_open = next;
      break;
    }
  }
  return result;
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
                static_cast<long long>(millis / 1'000 % 60),
                static_cast<long long>(millis % 1'000));
  return buffer;
}

}  // namespace openport::md
