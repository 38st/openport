#include <gtest/gtest.h>
#include <sys/wait.h>

#include <cstdio>
#include <string>

namespace {
void rejects(const std::string& application, const std::string& args, const std::string& reason) {
  const auto command =
      std::string("\"") + OPENPORT_APPS_DIR + "/" + application + "\" " + args + " 2>&1";
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
}  // namespace
