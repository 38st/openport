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
  EXPECT_EQ(format_timestamp(new_york_to_utc(Date{2026, 9, 22}, 16, 0)),
            "2026-09-22T20:00:00.000Z");
  EXPECT_EQ(format_timestamp(new_york_to_utc(Date{2026, 12, 18}, 9, 30)),
            "2026-12-18T14:30:00.000Z");
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

TEST(Time, RejectsImpossibleDatesFieldsAndTimestampOverflow) {
  for (const auto text :
       {"2026-02-31T12:00:00", "2025-02-29T12:00:00", "2026-04-31T12:00:00", "2026-00-01T12:00:00",
        "2026-01-00T12:00:00", "2026-01-01T-1:00:00", "2026-01-01T12:-1:00", "2026-01-01T12:00:-1",
        "2026-01-01T24:00:00", "2026-01-01T12:60:00", "2026-01-01T12:00:60", "9999-01-01T00:00:00",
        "1600-01-01T00:00:00", "-001-01-01T00:00:00"}) {
    EXPECT_FALSE(parse_datetime(text, Zone::Utc)) << text;
  }
  EXPECT_TRUE(parse_datetime("2028-02-29T12:00:00", Zone::Utc));
}

TEST(Time, AppliesExplicitZonesAndRejectsUnrecognisedSuffixes) {
  for (auto zone : {Zone::Utc, Zone::NewYork}) {
    EXPECT_EQ(parse_datetime("2026-09-22T15:30:00-04:00", zone),
              parse_datetime("2026-09-22T19:30:00Z", Zone::Utc));
    EXPECT_EQ(parse_datetime("2026-09-22T15:30:00+05:30", zone),
              parse_datetime("2026-09-22T10:00:00Z", Zone::Utc));
    EXPECT_EQ(parse_datetime("2026-09-22T15:30:00Z", zone),
              parse_datetime("2026-09-22T15:30:00", Zone::Utc));
    for (const auto suffix :
         {"junk", "EST", "+24:00", "-04:60", "+0400", ".", ".a", ".1234567890", "Zjunk"})
      EXPECT_FALSE(parse_datetime(std::string("2026-09-22T15:30:00") + suffix, zone)) << suffix;
  }
}

TEST(Time, RejectsSpringGapAndUsesFirstFallOccurrence) {
  EXPECT_FALSE(parse_datetime("2026-03-08T02:30:00", Zone::NewYork));
  EXPECT_EQ(parse_datetime("2026-11-01T01:30:00", Zone::NewYork),
            parse_datetime("2026-11-01T05:30:00Z", Zone::Utc));
}

TEST(Time, ChecksNanosecondRangeIncludingFractionsAndOffsets) {
  using openport::md::Timestamp;
  EXPECT_EQ(parse_datetime("2262-04-11T23:47:16.854775807Z", Zone::Utc),
            std::numeric_limits<Timestamp>::max());
  EXPECT_FALSE(parse_datetime("2262-04-11T23:47:16.854775808Z", Zone::Utc));
  EXPECT_EQ(parse_datetime("1677-09-21T00:12:43.145224192Z", Zone::Utc),
            std::numeric_limits<Timestamp>::min());
  EXPECT_FALSE(parse_datetime("1677-09-21T00:12:43.145224191Z", Zone::Utc));
  EXPECT_FALSE(parse_datetime("2262-04-11T23:00:00-01:00", Zone::Utc));
  EXPECT_EQ(new_york_to_utc({2026, 1, 257}, 12, 0), openport::md::kInvalidTimestamp);
  EXPECT_EQ(new_york_to_utc({2026, 3, 8}, 2, 30), openport::md::kInvalidTimestamp);
  EXPECT_EQ(new_york_to_utc({9999, 1, 1}, 12, 0), openport::md::kInvalidTimestamp);
  EXPECT_EQ(new_york_to_utc({2026, 1, 1}, -1, 0), openport::md::kInvalidTimestamp);
}

TEST(Time, MarketSessionNotesBoundariesAndNextOpen) {
  using openport::md::market_session;
  auto session = [](Date date, int h, int m) {
    return market_session(new_york_to_utc(date, h, m));
  };
  EXPECT_EQ(session({2026, 9, 22}, 9, 29).note, "closed (pre-market)");
  EXPECT_EQ(session({2026, 9, 22}, 9, 29).next_open, new_york_to_utc({2026, 9, 22}, 9, 30));
  EXPECT_TRUE(session({2026, 9, 22}, 9, 30).open);
  EXPECT_FALSE(session({2026, 9, 22}, 9, 30).next_open);
  EXPECT_EQ(session({2026, 9, 22}, 12, 0).note, "open");
  EXPECT_TRUE(session({2026, 9, 22}, 15, 59).open);
  EXPECT_EQ(session({2026, 9, 22}, 16, 0).note, "closed (after hours)");
  EXPECT_EQ(session({2026, 9, 22}, 16, 0).next_open, new_york_to_utc({2026, 9, 23}, 9, 30));
  EXPECT_EQ(session({2026, 9, 26}, 12, 0).note, "closed (weekend)");
  EXPECT_EQ(session({2026, 9, 26}, 12, 0).next_open, new_york_to_utc({2026, 9, 28}, 9, 30));
  EXPECT_EQ(session({2026, 11, 26}, 12, 0).note, "closed (holiday: Thanksgiving Day)");
  EXPECT_EQ(session({2026, 11, 26}, 12, 0).next_open, new_york_to_utc({2026, 11, 27}, 9, 30));
  EXPECT_EQ(session({2026, 11, 27}, 12, 59).note, "open, early close 13:00 ET");
  EXPECT_EQ(session({2026, 11, 27}, 13, 0).note, "closed (after hours)");
  EXPECT_EQ(session({2026, 11, 27}, 13, 0).next_open, new_york_to_utc({2026, 11, 30}, 9, 30));
  // A UTC Saturday can still be Friday in New York, including across DST changes.
  EXPECT_EQ(market_session(*parse_datetime("2026-03-07T01:00:00Z", Zone::Utc)).note,
            "closed (after hours)");
  EXPECT_EQ(market_session(*parse_datetime("2026-03-07T01:00:00Z", Zone::Utc)).next_open,
            parse_datetime("2026-03-09T13:30:00Z", Zone::Utc));
  EXPECT_EQ(market_session(*parse_datetime("2026-10-31T01:00:00Z", Zone::Utc)).next_open,
            parse_datetime("2026-11-02T14:30:00Z", Zone::Utc));
}

TEST(Time, CalendarCoversPublishedHolidaysAndOnlyPublishedEarlyCloses) {
  using openport::md::market_session;
  const int holidays[][11] = {{101, 109, 120, 217, 418, 526, 619, 704, 901, 1127, 1225},
                              {101, 119, 216, 403, 525, 619, 703, 907, 1126, 1225, 0},
                              {101, 118, 215, 326, 531, 618, 705, 906, 1125, 1224, 0},
                              {117, 221, 414, 529, 619, 704, 904, 1123, 1225, 0, 0}};
  for (int y = 2025; y <= 2028; ++y)
    for (int md : holidays[y - 2025]) {
      if (md == 0) continue;
      const auto s = market_session(new_york_to_utc({y, md / 100, md % 100}, 12, 0));
      EXPECT_FALSE(s.open) << y << ' ' << md;
      EXPECT_EQ(s.note.find("closed (holiday:"), 0u) << s.note;
    }
  for (Date d : {Date{2025, 7, 3},
                 {2025, 11, 28},
                 {2025, 12, 24},
                 {2026, 11, 27},
                 {2026, 12, 24},
                 {2027, 11, 26},
                 {2028, 7, 3},
                 {2028, 11, 24}}) {
    EXPECT_EQ(market_session(new_york_to_utc(d, 12, 0)).note, "open, early close 13:00 ET");
    EXPECT_FALSE(market_session(new_york_to_utc(d, 13, 0)).open);
  }
  // July 2 is not an early close in 2026; New Year 2028 has no observed Friday closure.
  EXPECT_TRUE(market_session(new_york_to_utc({2026, 7, 2}, 15, 0)).open);
  EXPECT_TRUE(market_session(new_york_to_utc({2027, 12, 31}, 12, 0)).open);
  // Outside the published range use weekday rules, not extrapolated holidays.
  EXPECT_TRUE(market_session(new_york_to_utc({2029, 1, 1}, 12, 0)).open);
  EXPECT_FALSE(market_session(new_york_to_utc({2029, 1, 6}, 12, 0)).open);
}

TEST(Time, YearFractionPreservesNanosecondsWithoutSignedOverflow) {
  const auto t = new_york_to_utc({2026, 9, 22}, 12, 0);
  EXPECT_DOUBLE_EQ(openport::md::years_between(t, t + 1), 1 / openport::md::kNanosPerYear);
  EXPECT_GT(openport::md::years_between(std::numeric_limits<openport::md::Timestamp>::min(),
                                        std::numeric_limits<openport::md::Timestamp>::max()),
            584);
}

}  // namespace
