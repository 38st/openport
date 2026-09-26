#include <gtest/gtest.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <string>

#include "support/recording.hpp"
#include "support/scripted_market.hpp"
#include "openport/trading/journal.hpp"

namespace {
/// A private HOME for launched binaries, so a daemon that gets far enough to start never
/// touches the user's real ~/.openport paper journal.
std::string isolated_home() {
  static const auto home = [] {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-cli-home-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    return pattern;
  }();
  return "HOME='" + home + "' ";
}

void rejects(const std::string& application, const std::string& args, const std::string& reason) {
  const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR + "/" + application + "\" " +
                       args + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
  const auto status = pclose(pipe);
  ASSERT_TRUE(WIFEXITED(status)) << output;
  EXPECT_EQ(WEXITSTATUS(status), 2) << output;
  EXPECT_NE(output.find(reason), std::string::npos) << output;
}

TEST(Cli, DaemonRejectsMalformedRangesUnknownFlagsAndStartupFailures) {
  for (const auto* port : {"0", "65536", "-1", "80junk", "1.2", "99999999999999999999"})
    rejects("openportd", std::string("--port ") + port, "--port");
  for (const auto* expiry : {"-1", "1x", "1.2"})
    rejects("openportd", std::string("--expiries ") + expiry, "--expiries");
  for (const auto* window : {"nan", "inf", "1.1", "0.1x", "-0.1"})
    rejects("openportd", std::string("--window ") + window, "--window");
  for (const auto* rate : {"nan", "inf", "0.251", "-0.051", "0.04x"})
    rejects("openportd", std::string("--rate ") + rate, "--rate");
  for (const auto* rate : {"-0.05", "0.25", "0.045"})
    rejects("openportd", std::string("--rate ") + rate + " --provider missing", "unknown provider");
  rejects("openportd", "--poll-seconds 0", "poll_seconds");
  rejects("openportd", "--poll-seconds 1x", "poll_seconds");
  rejects("openportd", "--option unknown=1", "unknown provider option");
  rejects("openportd", "--unknown value", "unknown option");
  rejects("openportd", "--address invalid-address", "openportd:");
  rejects("openportd", "--provider missing", "unknown provider");
  rejects("openportd", "--allowed-origin https://host/path", "invalid allowed origin");
  rejects("openportd", "--provider databento --window 0.1", "whole chain upstream");
}

TEST(Cli, ProbeRejectsBadValuesUnknownFlagsAndDatabentoFilters) {
  rejects("openport-probe", "cboe SPY --window nan", "--window");
  rejects("openport-probe", "cboe SPY --window 1x", "--window");
  rejects("openport-probe", "cboe SPY --expiries -1", "--expiries");
  rejects("openport-probe", "cboe SPY --seconds 0", "--seconds");
  rejects("openport-probe", "cboe SPY --quotes 0", "--quotes");
  rejects("openport-probe", "cboe SPY --unknown", "unknown option");
  rejects("openport-probe", "cboe SPY -x", "unknown option");
  rejects("openport-probe", "cboe SPY --window", "missing value");
  rejects("openport-probe", "missing SPY", "unknown provider");
  rejects("openport-probe", "databento SPX --expiries 1", "whole chain upstream");
}
TEST(Cli, ReplayAndRecordValidationWorksInBothApplications) {
  openport::test::RecordingFile file;
  openport::test::record_events(file.path, {});
  const auto path = " --option file='" + file.path.string() + "'";
  rejects("openportd", "--provider replay", "file=PATH is required");
  rejects("openport-probe", "replay SPX", "file=PATH is required");
  for (const auto& option : {"speed=2", "speed=nan", "loop=yes", "unknown=1"}) {
    const auto reason = std::string(option).substr(0, std::string(option).find('='));
    rejects("openportd", "--provider replay" + path + " --option " + option, reason);
    rejects("openport-probe", "replay SPX" + path + " --option " + option, reason);
  }
  rejects("openportd", "--provider replay --symbols QQQ" + path, "file contains: SPX, SPY");
  rejects("openport-probe", "replay QQQ" + path, "file contains: SPX, SPY");
  rejects("openportd", "--provider replay" + path + " --record '" + file.path.string() + "'",
          "File exists");
  rejects("openport-probe", "replay SPX" + path + " --record '" + file.path.string() + "'",
          "File exists");
  rejects("openportd", "--record ''", "nonempty path");
  rejects("openport-probe", "cboe SPX --record ''", "nonempty path");
  rejects("openport-probe", "replay SPX --option missing-equals", "KEY=VALUE");
}

TEST(Cli, ProbeCanReplayAndRecordANewFileWithoutNetwork) {
  using namespace openport;
  test::RecordingFile source, recorded;
  test::record_events(source.path,
                      {md::ContractDefinition{0, *md::parse_osi("SPXW261022C05000000")},
                       md::OptionQuote{0, 100, 100, 101, 10, 10},
                       md::UnderlyingQuote{"SPX", 100, 5000, 5001, 5000.5},
                       md::ProviderStatus{100, md::FeedState::Live, "snapshot complete", "SPX"}});
  const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR +
                       "/openport-probe\" replay SPX --option file='" + source.path.string() +
                       "' --option speed=max --option loop=off --record '" +
                       recorded.path.string() + "' 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
  const auto status = pclose(pipe);
  ASSERT_TRUE(WIFEXITED(status)) << output;
  EXPECT_EQ(WEXITSTATUS(status), 0) << output;
  EXPECT_NE(output.find("replay (synthetic)"), std::string::npos) << output;
  EXPECT_NE(output.find("quotes=1"), std::string::npos) << output;
  md::RecordingReader reader(recorded.path);
  EXPECT_EQ(reader.header().provider, "replay (synthetic)");
  EXPECT_EQ(reader.header().subscription.underlyings, std::vector<std::string>{"SPX"});
  std::size_t events = 0;
  while (reader.next()) ++events;
  EXPECT_GE(events, 4u);
  EXPECT_TRUE(reader.diagnostic().empty());
}

TEST(Cli, DaemonReportsJournalLockedByAnotherProcessBeforeWebStartupFails) {
  using namespace openport;
  test::RecordingFile source, journal_file;
  test::record_events(source.path, {});
  const auto journal = trading::FileJournal::create(journal_file.path.string());
  journal->append(0, "test", "{}");
  const auto head = journal->head();
  const auto command = "--provider replay --symbols SPX,SPY --option file='" + source.path.string() +
      "' --option speed=max --paper-journal '" + journal_file.path.string() +
      "' --address invalid-address";
  rejects("openportd", command, "JOURNAL_LOCKED: paper journal '" + journal_file.path.string() +
      "' is in use by another openportd; use --paper-journal to choose another file or --no-paper");
  const auto recovery = trading::FileJournal::read(journal_file.path.string(), head);
  EXPECT_EQ(recovery.records.size(), 1u);
  EXPECT_FALSE(recovery.truncated_final_line);
  journal->append(1, "still-owned", "{}");
  EXPECT_EQ(journal->sequence(), 2u);
}

struct Finished { int status = -1; std::string output; };
Finished run_daemon(const std::string& args) {
  const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR + "/openportd\" " + args + " 2>&1";
  Finished finished;
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) return finished;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) finished.output += buffer;
  const auto status = pclose(pipe);
  if (WIFEXITED(status)) finished.status = WEXITSTATUS(status);
  return finished;
}

TEST(Cli, DaemonPrintsItsVersion) {
  const auto result = run_daemon("--version");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_TRUE(std::regex_match(result.output, std::regex(R"(openportd \d+\.\d+\.\d+\n)"))) << result.output;
}

TEST(Cli, DaemonDefaultsToTheIndexAndFourEtfsAndAllowsAnOverride) {
  using namespace openport;
  const std::vector<std::string> symbols{"SPX", "SPY", "QQQ", "IWM", "DIA"};
  test::RecordingFile source;
  auto header = test::recording_header();
  header.subscription.underlyings = symbols;
  test::record_events(source.path, {}, header);
  for (const std::string flags : {"", " --symbols IWM,DIA"}) {
    test::RecordingFile recorded;
    // Record the subscription before an invalid address ends startup, without a listener or network feed.
    const auto result = run_daemon("--provider replay --option file='" + source.path.string() +
        "' --option speed=max --no-paper --record '" + recorded.path.string() +
        "' --address invalid-address" + flags);
    EXPECT_EQ(result.status, 2) << result.output;
    EXPECT_NE(result.output.find("openportd:"), std::string::npos) << result.output;
    EXPECT_EQ(result.output.find("replay: unknown symbol"), std::string::npos) << result.output;
    md::RecordingReader reader(recorded.path);
    EXPECT_EQ(reader.header().subscription.underlyings,
              (flags.empty() ? symbols : std::vector<std::string>{"IWM", "DIA"}));
    EXPECT_EQ(reader.header().subscription.max_expiries, 0);
    EXPECT_DOUBLE_EQ(reader.header().subscription.strike_window, 0);
  }
  const auto help = run_daemon("--help");
  EXPECT_EQ(help.status, 2);
  EXPECT_NE(help.output.find("[--symbols SPX,SPY,QQQ,IWM,DIA]"), std::string::npos) << help.output;
}

TEST(Cli, DaemonCompactsJournalsFromEarlierBuildsAndKeepsTheOriginals) {
  using namespace openport;
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-compact-XXXXXX").string();
  ASSERT_NE(mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  std::filesystem::create_directories(directory / "accounts");
  const auto main = directory / "paper-journal.jsonl";
  const auto swing = directory / "accounts" / "swing.jsonl";
  const auto fresh = directory / "accounts" / "fresh.jsonl";
  // Journals as a build before the compact format wrote them, and one written since.
  test::ScriptedMarket f;
  std::string expected;
  for (const auto& file : {main, swing, fresh}) {
    const auto current = file.string() + ".current";
    {
      trading::TradingSession s({}, f.time, trading::FileJournal::create(current));
      f.seed(s);
      for (int i = 0; i < 4; ++i) {
        f.next();
        s.on_quotes({f.quote()}, {f.valuation()}, f.time);
        ASSERT_TRUE(s.submit(f.market("buy-" + std::to_string(i)), f.time).decision.ok());
      }
      if (file == main) expected = s.snapshot_json();
    }
    if (file == fresh) {
      std::filesystem::rename(current, file);
    } else {
      trading::TradingSession::expand(trading::FileJournal::read(current), *trading::FileJournal::create(file.string()));
      std::filesystem::remove(current);
    }
  }
  const auto before = std::filesystem::file_size(main);
  const auto args = "--compact-journals --paper-journal '" + main.string() + "'";
  {
    // A journal in use stays as it is; the others are still rewritten.
    const auto held = trading::FileJournal::resume(swing.string());
    const auto result = run_daemon(args);
    EXPECT_EQ(result.status, 1) << result.output;
    EXPECT_NE(result.output.find(swing.string() + ": left as it was: JOURNAL_LOCKED"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find(main.string() + ": "), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("; the original is paper-journal.jsonl.bak"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find(fresh.string() + ": already compact"), std::string::npos) << result.output;
  }
  EXPECT_EQ(std::filesystem::file_size(directory / "paper-journal.jsonl.bak"), before);
  EXPECT_LT(std::filesystem::file_size(main) * 2, before);
  EXPECT_EQ(trading::TradingSession::recover(trading::FileJournal::read(main.string())).snapshot_json(), expected);
  EXPECT_EQ(trading::TradingSession::recover(trading::FileJournal::read((directory / "paper-journal.jsonl.bak").string())).snapshot_json(), expected);
  const auto again = run_daemon(args);
  EXPECT_EQ(again.status, 0) << again.output;
  EXPECT_NE(again.output.find(main.string() + ": already compact"), std::string::npos) << again.output;
  EXPECT_NE(again.output.find("; the original is swing.jsonl.bak"), std::string::npos) << again.output;
  EXPECT_FALSE(std::filesystem::exists(directory / "paper-journal.jsonl.bak2"));
  for (const auto& entry : std::filesystem::directory_iterator(directory / "accounts"))
    EXPECT_EQ(entry.path().string().find(".compacting"), std::string::npos) << entry.path();
  std::filesystem::create_directories(directory / "empty");
  const auto missing = run_daemon("--compact-journals --paper-journal '" + (directory / "empty" / "none.jsonl").string() + "'");
  EXPECT_EQ(missing.status, 0) << missing.output;
  EXPECT_NE(missing.output.find("no paper journals"), std::string::npos) << missing.output;
  std::filesystem::remove_all(directory);
}

}  // namespace
