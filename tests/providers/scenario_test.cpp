#include "support/recording.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#include "openport/providers/scenario.hpp"
#include "openport/pricing/implied_vol.hpp"

namespace {
using namespace openport;
using nlohmann::json;
json valid() {
  return {{"id", "custom"}, {"title", "Custom"}, {"description", "A simulated day."}, {"goal", "Manage risk."},
          {"symbols", {"SPX"}}, {"session", "regular"}, {"date", "2026-09-16"}, {"seed", 17}, {"generator", 1},
          {"drift", {{0.5, 0.005}, {1.0, 0.0}}}, {"volatility", 0.1}, {"iv_shift", 0}, {"spot_vol", -2}};
}
void write(const std::filesystem::path& file, const json& data) { std::ofstream(file) << data; }

TEST(Scenarios, BuiltinsAreValidAndUsersOverrideAfterThem) {
  EXPECT_EQ(providers::builtin_scenarios().size(), 14U);
  for (const auto& s : providers::builtin_scenarios()) {
    EXPECT_FALSE(s.goal.empty());
    EXPECT_EQ(s.generator, 1);
  }
  test::RecordingFile directory;
  const auto user = directory.directory / "user";
  std::filesystem::create_directory(user);
  write(user / "addition.json", valid());
  auto override = valid(); override["id"] = "reversal"; override["title"] = "Replacement";
  write(user / "override.json", override);
  auto bad = valid(); bad["generator"] = 9;
  write(user / "bad.json", bad);
  std::vector<std::string> errors;
  const auto loaded = providers::load_scenarios(user, [&](const auto& error) { errors.push_back(error); });
  ASSERT_EQ(loaded.size(), 15U);
  EXPECT_EQ(loaded.front().id, "trend");
  EXPECT_EQ(loaded[13].id, "custom");
  EXPECT_EQ(loaded.back().id, "reversal");
  EXPECT_EQ(loaded.back().title, "Replacement");
  EXPECT_NE(providers::builtin_scenarios().front().title, "Replacement");
  ASSERT_EQ(errors.size(), 1U);
  EXPECT_NE(errors.front().find("bad.json: generator"), std::string::npos);
  // Embedded texts and user files use the same validation and source diagnostics.
  try { (void)providers::parse_scenario(bad.dump(), "scenarios/invalid.json"); FAIL(); }
  catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("scenarios/invalid.json: generator"), std::string::npos);
  }
}

TEST(Scenarios, RejectsInvalidFieldsWithFileAndFieldNames) {
  test::RecordingFile directory;
  const auto path = directory.directory / "invalid.json";
  const std::vector<std::pair<std::string, json>> invalid{
      {"generator", 2}, {"generator", 1.0}, {"id", "../x"}, {"seed", -1}, {"seed", 1.5},
      {"symbols", {"SPX", "SPX"}}, {"symbols", {"FAKE"}}, {"session", "day"}, {"date", "2026-09-19"},
      {"drift", {{0.5, 0.1}, {0.4, 0.1}}}, {"drift", {{0.5, 0.1}}}, {"volatility", -0.1},
      {"iv_shift", "high"}, {"spot_vol", 1}, {"typo", true},
      {"events", {{{"type", "pin"}, {"at", "25:00"}, {"strike", 6000}}}},
      {"events", {{{"type", "crush"}, {"at", "10:00"}, {"iv", 0.1}}}},
      {"events", {{{"type", "gap"}, {"at", "10:00"}, {"move", 0.01}}}},
      {"events", {{{"type", "spike"}, {"at", "10:00"}, {"move", 0.01}}}}};
  for (const auto& [field, value] : invalid) {
    auto data = valid(); data[field] = value; write(path, data);
    try { (void)providers::read_scenario(path); FAIL() << field; }
    catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find("invalid.json"), std::string::npos);
      EXPECT_NE(std::string(error.what()).find(field == "events" ? "" : field), std::string::npos);
    }
  }
  auto data = valid(); data.erase("drift"); write(path, data);
  EXPECT_THROW((void)providers::read_scenario(path), std::invalid_argument);
  EXPECT_EQ(providers::scenario_time("20:15", {2026, 9, 16}, true), md::new_york_to_utc({2026, 9, 15}, 20, 15));
  EXPECT_EQ(providers::scenario_time("08:00", {2026, 9, 16}, true), md::new_york_to_utc({2026, 9, 16}, 8, 0));
}

struct Prices {
  std::map<md::Timestamp, double> spot;
  std::map<std::pair<md::Timestamp, md::Date>, double> iv;
};
Prices prices(const providers::Scenario& scenario, std::uint64_t seed) {
  test::RecordingFile file;
  providers::write_scenario_recording(file.path, scenario, scenario.date, seed);
  md::RecordingReader reader(file.path);
  Prices out;
  std::map<md::InstrumentId, md::OptionContract> contracts;
  double spot = 6000;
  while (const auto event = reader.next()) {
    if (const auto* def = std::get_if<md::ContractDefinition>(&event->event)) contracts[def->id] = def->contract;
    if (const auto* quote = std::get_if<md::UnderlyingQuote>(&event->event)) out.spot[quote->ts] = spot = quote->last;
    if (const auto* quote = std::get_if<md::OptionQuote>(&event->event)) {
      const auto& contract = contracts.at(quote->id);
      if (contract.strike != 6000 || contract.type != pricing::OptionType::Call || quote->ask == 0) continue;
      const auto iv = pricing::implied_vol_bsm((quote->bid + quote->ask) / 2, contract.type, spot, contract.strike,
          md::years_between(quote->ts, contract.expiry_time()), 0.04, 0.013);
      if (iv.ok()) out.iv[{quote->ts, contract.expiry}] = iv.vol;
    }
  }
  return out;
}

TEST(Scenarios, GapCrushSpikeAndPinRepriceTheChainAndSeedsRepeat) {
  auto scenario = providers::builtin_scenarios().front();
  scenario.symbols = {"SPX"}; scenario.drift = {{1, 0.004}};
  const auto baseline = prices(scenario, 17);
  EXPECT_EQ(prices(scenario, 17).spot, baseline.spot);
  EXPECT_NE(prices(scenario, 18).spot, baseline.spot);
  scenario.events = {{"gap", "", .01, 0, 0}};
  const auto gap = prices(scenario, 17);
  EXPECT_NEAR(gap.spot.begin()->second / baseline.spot.begin()->second, std::exp(.01), .00001);
  scenario.events = {{"crush", "10:30", 0, -.05, 0}};
  const auto crush = prices(scenario, 17);
  const auto at = md::new_york_to_utc(scenario.date, 10, 30);
  int expiries = 0;
  double short_drop = 0, long_drop = 0;
  for (const auto& [key, iv] : baseline.iv) {
    if (key.first == at && crush.iv.contains(key)) {
      const double drop = iv - crush.iv.at(key);
      EXPECT_GT(drop, .009);
      if (key.second == scenario.date) short_drop = drop;
      if (key.second.month == 10) long_drop = drop;
      ++expiries;
    }
    if (key.first < at && crush.iv.contains(key)) EXPECT_DOUBLE_EQ(iv, crush.iv.at(key));
  }
  EXPECT_EQ(expiries, 5);
  EXPECT_GT(short_drop, long_drop);
  scenario.events = {{"spike", "10:30", .01, .04, 0}};
  const auto spike = prices(scenario, 17);
  EXPECT_GT(spike.spot.at(at + 5 * md::kNanosPerMinute), baseline.spot.at(at + 5 * md::kNanosPerMinute) * 1.009);
  EXPECT_GT(spike.iv.at({at, scenario.date}), baseline.iv.at({at, scenario.date}) + .03);
  scenario.events = {{"pin", "14:30", 0, 0, 6000}};
  const auto pin = prices(scenario, 17);
  EXPECT_DOUBLE_EQ(pin.spot.rbegin()->second, 6000);
  EXPECT_GT(std::abs(baseline.spot.rbegin()->second - 6000), 1);
  scenario.generator = 2;
  test::RecordingFile file;
  EXPECT_THROW(providers::write_scenario_recording(file.path, scenario, scenario.date, 17), std::invalid_argument);
}
}  // namespace
