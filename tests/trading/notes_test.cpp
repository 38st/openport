#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;

void expect_invalid(TradingSession& s, std::string note, std::vector<std::string> tags, Timestamp time) {
  const auto version = s.snapshot()->account_version;
  try {
    (void)s.annotate(1, std::move(note), std::move(tags), time);
    ADD_FAILURE() << "expected INVALID_NOTE";
  } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::INVALID_NOTE) << error.what(); }
  EXPECT_EQ(s.snapshot()->account_version, version);  // nothing recorded
}

TEST(TradingNotes, TradesTakeANoteAndTagsThatSurviveRecovery) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-notes-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string expected;
  {
    TradingSession s({}, f.time, FileJournal::create(path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
    const auto trade = s.snapshot()->recent_fills.front().id;
    ASSERT_TRUE(s.annotate(trade, "  Faded the open; stop was too tight.\n", {" Breakout ", "0DTE", "breakout"}, f.time).decision.ok());
    const auto& note = s.snapshot()->annotations.at(std::to_string(trade));
    EXPECT_EQ(note.note, "Faded the open; stop was too tight.");
    EXPECT_EQ(note.tags, (std::vector<std::string>{"breakout", "0dte"}));
    EXPECT_EQ(note.time, f.time);
    // The closing fill does not open a trade.
    EXPECT_EQ(s.annotate(s.snapshot()->recent_fills.back().id, "no", {}, f.time).decision.code, Reason::UNKNOWN_TRADE);
    EXPECT_EQ(s.annotate(99, "no", {}, f.time).decision.code, Reason::UNKNOWN_TRADE);
    // Notes are kept whatever the account's state.
    s.trip_kill("review", f.time);
    ASSERT_TRUE(s.annotate(trade, "Reviewed.", {"breakout"}, f.time).decision.ok());
    expected = s.snapshot_json();
  }
  auto s = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  EXPECT_EQ(s.snapshot()->annotations.at("1").note, "Reviewed.");
  // An empty note without tags clears it.
  ASSERT_TRUE(s.annotate(1, " ", {}, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->annotations.empty());
  std::filesystem::remove_all(directory);
}

TEST(TradingNotes, NotesAndTagsMustBeShortPlainText) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  ASSERT_TRUE(s.annotate(1, std::string(2000, 'x'), {std::string(32, 't')}, f.time).decision.ok());
  expect_invalid(s, std::string(2001, 'x'), {}, f.time);
  expect_invalid(s, "bell\a", {}, f.time);
  expect_invalid(s, "bad \xff byte", {}, f.time);
  expect_invalid(s, "", {std::string(33, 't')}, f.time);
  expect_invalid(s, "", {"a,b"}, f.time);
  expect_invalid(s, "", {" "}, f.time);
  expect_invalid(s, "", {"line\nbreak"}, f.time);
  expect_invalid(s, "", {"1", "2", "3", "4", "5", "6", "7", "8", "9"}, f.time);
  // Tags repeated after lowercasing count once.
  EXPECT_TRUE(s.annotate(1, "", {"a", "A", "b", "c", "d", "e", "f", "g", "h"}, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->annotations.at("1").tags.size(), 8U);
}

}  // namespace
}  // namespace openport::trading
