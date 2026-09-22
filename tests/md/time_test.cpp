#include "openport/md/time.hpp"

#include <gtest/gtest.h>

namespace {

using openport::md::Date;
using openport::md::format_timestamp;
using openport::md::new_york_to_utc;
using openport::md::new_york_utc_offset_hours;
using openport::md::parse_datetime;
using openport::md::Zone;

TEST(Time, CivilDateRoundTrips) {
  EXPECT_EQ(openport::md::days_since_epoch(Date{1970, 1, 1}), 0);
  for (std::int64_t days = -1000; days < 40000; days += 37) {
    EXPECT_EQ(openport::md::days_since_epoch(openport::md::date_from_days(days)), days);
  }
  EXPECT_EQ(openport::md::weekday(Date{2026, 9, 22}), 2);  // a Tuesday
  EXPECT_EQ(openport::md::weekday(Date{2026, 3, 8}), 0);   // a Sunday
}

TEST(Time, NewYorkDaylightSavingBoundaries) {
  // 2026: daylight saving starts Sunday 8 March at 02:00 and ends Sunday 1 November at 02:00.
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 3, 7}, 12), -5);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 3, 8}, 1), -5);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 3, 8}, 3), -4);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 7, 4}, 12), -4);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 11, 1}, 1), -4);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 11, 1}, 2), -5);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 12, 18}, 16), -5);
}

TEST(Time, MarketCloseInUtc) {
  EXPECT_EQ(format_timestamp(new_york_to_utc(Date{2026, 9, 22}, 16, 0)), "2026-09-22T20:00:00.000Z");
  EXPECT_EQ(format_timestamp(new_york_to_utc(Date{2026, 12, 18}, 9, 30)), "2026-12-18T14:30:00.000Z");
}

TEST(Time, ParsesUtcAndNewYorkWallClock) {
  EXPECT_EQ(format_timestamp(*parse_datetime("2026-09-22 19:48:44", Zone::Utc)),
            "2026-09-22T19:48:44.000Z");
  EXPECT_EQ(format_timestamp(*parse_datetime("2026-09-22T15:33:42", Zone::NewYork)),
            "2026-09-22T19:33:42.000Z");
  EXPECT_EQ(format_timestamp(*parse_datetime("2026-09-22T15:33:42.125", Zone::NewYork)),
            "2026-09-22T19:33:42.125Z");
  EXPECT_EQ(*parse_datetime("2026-09-22 19:48:44.000000001", Zone::Utc) % 1'000'000'000, 1);
  EXPECT_FALSE(parse_datetime("2026-09-22", Zone::Utc));
  EXPECT_FALSE(parse_datetime("2026/09/22 19:48:44", Zone::Utc));
  EXPECT_FALSE(parse_datetime("2026-13-22 19:48:44", Zone::Utc));
}

TEST(Time, YearFractionIsActual365) {
  const auto from = new_york_to_utc(Date{2026, 1, 1}, 0, 0);
  const auto to = new_york_to_utc(Date{2027, 1, 1}, 0, 0);
  EXPECT_DOUBLE_EQ(openport::md::years_between(from, to), 1.0);
}

}  // namespace
