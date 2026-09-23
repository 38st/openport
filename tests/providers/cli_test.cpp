#include <gtest/gtest.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "support/recording.hpp"

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

}  // namespace
