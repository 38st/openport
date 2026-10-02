#include "support/recording.hpp"

#include <fstream>
#include <functional>
#include <map>
#include <set>
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
  EXPECT_EQ(providers::builtin_scenarios().size(), 18U);
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
  ASSERT_EQ(loaded.size(), 19U);
  EXPECT_EQ(loaded.front().id, "trend");
  EXPECT_EQ(loaded[17].id, "custom");
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

json several() {
  auto data = valid();
  for (const auto* field : {"session", "drift", "volatility", "iv_shift", "spot_vol"}) data.erase(field);
  data["symbols"] = {"SPX", "SPY"};
  data["volatility"] = 0.1;
  data["sessions"] = json::array({
      {{"session", "regular"}, {"drift", {{1, 0.004}}}, {"iv_shift", 0}, {"spot_vol", -2}},
      {{"session", "curb"}, {"drift", {{1, 0}}}, {"iv_shift", 0}, {"spot_vol", -2}},
      {{"session", "overnight"}, {"drift", {{1, -0.002}}}, {"iv_shift", 0.01}, {"spot_vol", -2}, {"volatility", 0.05},
       {"events", {{{"type", "spike"}, {"at", "21:00"}, {"move", -0.002}, {"iv", 0.01}}}}},
      {{"session", "regular"}, {"drift", {{1, 0.001}}}, {"iv_shift", 0}, {"spot_vol", -2},
       {"events", {{{"type", "gap"}, {"move", -0.003}}}}, {"dividends", {{{"symbol", "SPY"}, {"per_share", 1.75}}}}}});
  return data;
}

TEST(Scenarios, SeveralSessionsFollowEachOtherAcrossTradingDates) {
  const auto scenario = providers::parse_scenario(several().dump(), "several.json");
  ASSERT_EQ(scenario.sessions.size(), 4U);
  EXPECT_FALSE(scenario.overnight);
  EXPECT_EQ(scenario.sessions[2].volatility, 0.05);
  EXPECT_EQ(scenario.sessions[1].volatility, 0.1);
  EXPECT_EQ(scenario.sessions[2].iv_shift, 0.01);
  ASSERT_EQ(scenario.sessions[3].dividends.size(), 1U);
  EXPECT_EQ(scenario.sessions[3].dividends.front().per_share, 1.75);
  // Wednesday's regular session and curb, then Thursday's overnight and regular sessions.
  const md::Date wednesday{2026, 9, 16}, thursday{2026, 9, 17};
  const auto windows = providers::scenario_windows(scenario, scenario.date);
  ASSERT_EQ(windows.size(), 4U);
  EXPECT_EQ(windows[0].date, wednesday);
  EXPECT_EQ(windows[0].first, md::new_york_to_utc(wednesday, 9, 30));
  EXPECT_EQ(windows[0].close, md::new_york_to_utc(wednesday, 16, 0));
  EXPECT_EQ(windows[0].last, md::new_york_to_utc(wednesday, 16, 15));
  EXPECT_EQ(windows[1].date, wednesday);
  EXPECT_EQ(windows[1].first, md::new_york_to_utc(wednesday, 16, 15, 15));
  EXPECT_EQ(windows[1].last, md::new_york_to_utc(wednesday, 16, 59, 45));
  EXPECT_EQ(md::trading_date(windows[1].last), wednesday);
  EXPECT_EQ(windows[2].date, thursday);
  EXPECT_EQ(windows[2].first, md::new_york_to_utc(wednesday, 20, 15));
  EXPECT_EQ(windows[2].last, md::new_york_to_utc(thursday, 9, 25));
  EXPECT_EQ(windows[2].step, 60 * md::kNanosPerSecond);
  EXPECT_EQ(windows[3].date, thursday);
  EXPECT_EQ(windows[3].first, md::new_york_to_utc(thursday, 9, 30));
  EXPECT_EQ(providers::scenario_end(scenario, scenario.date), md::new_york_to_utc(thursday, 16, 15));
  // From a Friday, the next overnight session opens on Sunday evening for Monday.
  const md::Date friday{2026, 9, 18}, sunday{2026, 9, 20}, monday{2026, 9, 21};
  auto weekend = scenario;
  weekend.sessions.erase(weekend.sessions.begin() + 1);
  const auto carried = providers::scenario_windows(weekend, friday);
  ASSERT_EQ(carried.size(), 3U);
  EXPECT_EQ(carried[1].date, monday);
  EXPECT_EQ(carried[1].first, md::new_york_to_utc(sunday, 20, 15));
  EXPECT_EQ(carried[2].first, md::new_york_to_utc(monday, 9, 30));
  // Regular sessions alone jump from one date's close to the next date's open.
  auto days = scenario;
  days.sessions = {scenario.sessions[0], scenario.sessions[3], scenario.sessions[0]};
  const auto run = providers::scenario_windows(days, friday);
  EXPECT_EQ(run[1].date, monday);
  EXPECT_EQ(run[2].date, (md::Date{2026, 9, 22}));
  // The day after Thanksgiving closes early, without a curb.
  try { (void)providers::scenario_windows(scenario, {2026, 11, 27}); FAIL(); }
  catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("sessions[1].session"), std::string::npos) << error.what(); }
}

TEST(Scenarios, SeveralSessionsRejectImpossibleSequencesAndFields) {
  const std::vector<std::pair<std::string, std::function<void(json&)>>> invalid{
      {"sessions[0].session", [](json& d) { d["sessions"][0]["session"] = "curb"; }},
      {"sessions[2].session", [](json& d) { d["sessions"][2]["session"] = "curb"; }},
      {"sessions[3].session", [](json& d) { d["sessions"][3]["session"] = "overnight"; }},
      {"sessions[1].session", [](json& d) { d["sessions"][1]["session"] = "day"; }},
      {"drift", [](json& d) { d["drift"] = {{1, 0}}; }},
      {"events", [](json& d) { d["events"] = json::array(); }},
      {"sessions[0].drift", [](json& d) { d["sessions"][0].erase("drift"); }},
      {"sessions[0].volatility", [](json& d) { d.erase("volatility"); }},
      {"sessions[0].typo", [](json& d) { d["sessions"][0]["typo"] = 1; }},
      {"sessions", [](json& d) { d["sessions"] = json::array(); }},
      {"sessions[1].events.at", [](json& d) { d["sessions"][1]["events"] = {{{"type", "crush"}, {"at", "17:00"}, {"iv", -0.01}}}; }},
      {"sessions[2].events.pin", [](json& d) { d["sessions"][2]["events"] = {{{"type", "pin"}, {"at", "01:00"}, {"strike", 6000}}}; }},
      {"sessions[0].dividends", [](json& d) { d["sessions"][0]["dividends"] = d["sessions"][3]["dividends"]; }},
      {"sessions[3].dividends.symbol", [](json& d) { d["sessions"][3]["dividends"][0]["symbol"] = "QQQ"; }},
      {"sessions[3].dividends.symbol", [](json& d) { d["sessions"][2]["dividends"] = d["sessions"][3]["dividends"]; }},
      {"sessions[3].dividends.per_share", [](json& d) { d["sessions"][3]["dividends"][0]["per_share"] = 0; }},
      {"symbols", [](json& d) { d["symbols"] = {"SPY"}; }}};
  for (const auto& [field, change] : invalid) {
    auto data = several();
    change(data);
    try { (void)providers::parse_scenario(data.dump(), "several.json"); FAIL() << field; }
    catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find("several.json: " + field), std::string::npos) << field << ": " << error.what();
    }
  }
  auto many = several();
  for (int i = 0; i < 21; ++i) many["sessions"].push_back(many["sessions"][3]);
  EXPECT_THROW((void)providers::parse_scenario(many.dump(), "several.json"), std::invalid_argument);
}

TEST(Scenarios, SeveralSessionsPlayAsOneRecordingThatCarriesTheMarket) {
  auto data = several();
  data["volatility"] = 0;
  data["sessions"][0]["drift"] = {{1, 0.04}};
  data["sessions"][2]["volatility"] = 0;
  const auto scenario = providers::parse_scenario(data.dump(), "several.json");
  const auto windows = providers::scenario_windows(scenario, scenario.date);
  test::RecordingFile file;
  providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  md::RecordingReader reader(file.path);
  EXPECT_EQ(reader.header().started, windows.front().first);
  std::map<md::InstrumentId, md::OptionContract> contracts;
  std::map<md::InstrumentId, md::Timestamp> defined;
  std::map<md::InstrumentId, double> wednesday_volume, thursday_volume;
  std::vector<md::UnderlyingQuote> spx, spy;
  std::vector<md::UnderlyingClose> closes;
  std::set<std::string> closed_quotes, closed_snapshots;
  std::size_t closed_status = 0;
  md::Timestamp last = 0;
  const auto between = [](md::Timestamp t, const providers::ScenarioWindow& w) { return t >= w.first && t <= w.last; };
  while (const auto event = reader.next()) {
    last = std::max(last, event->received);
    const bool outside = between(event->received, windows[1]) || between(event->received, windows[2]);
    if (const auto* def = std::get_if<md::ContractDefinition>(&event->event)) {
      contracts[def->id] = def->contract;
      defined[def->id] = event->received;
    } else if (const auto* quote = std::get_if<md::UnderlyingQuote>(&event->event)) {
      (quote->symbol == "SPX" ? spx : spy).push_back(*quote);
      EXPECT_FALSE(outside && quote->ts >= windows.front().first) << quote->symbol;
    } else if (const auto* close = std::get_if<md::UnderlyingClose>(&event->event)) {
      closes.push_back(*close);
    } else if (const auto* option = std::get_if<md::OptionQuote>(&event->event)) {
      if (outside) closed_quotes.insert(contracts.at(option->id).underlying);
    } else if (const auto* complete = std::get_if<md::SnapshotComplete>(&event->event)) {
      if (outside) closed_snapshots.insert(complete->underlying);
    } else if (const auto* status = std::get_if<md::ProviderStatus>(&event->event)) {
      if (outside && status->message == "demo SPY: options closed") ++closed_status;
    } else if (const auto* volume = std::get_if<md::OptionVolume>(&event->event)) {
      auto& volumes = volume->ts < windows[2].first ? wednesday_volume : thursday_volume;
      if (!thursday_volume.contains(volume->id) || volume->ts < windows[2].first) volumes[volume->id] = volume->contracts;
    }
  }
  EXPECT_EQ(last, windows.back().last);
  // Outside the regular sessions only SPX options trade; SPY reports its options closed.
  EXPECT_EQ(closed_quotes, std::set<std::string>{"SPX"});
  EXPECT_EQ(closed_snapshots, std::set<std::string>{"SPX"});
  EXPECT_GT(closed_status, 0U);
  // The index prints through each regular session's close, and Thursday starts from
  // where Wednesday's sessions left it: the overnight drift and spike, then the gap.
  ASSERT_FALSE(spx.empty());
  const auto wednesday_close = std::find_if(spx.rbegin(), spx.rend(), [&](const auto& q) { return q.ts <= windows[0].close; });
  const auto thursday_open = std::find_if(spx.begin(), spx.end(), [&](const auto& q) { return q.ts >= windows[3].first; });
  ASSERT_TRUE(wednesday_close != spx.rend() && thursday_open != spx.end());
  EXPECT_EQ(wednesday_close->ts, windows[0].close);
  EXPECT_NEAR(wednesday_close->last, 6000 * std::exp(0.04), 0.01);
  EXPECT_EQ(thursday_open->ts, windows[3].first);
  EXPECT_NEAR(thursday_open->last, 6000 * std::exp(0.04 - 0.002 - 0.002 - 0.003), 0.01);
  // SPY trades lower by its dividend from its ex-date on.
  const auto spy_open = std::find_if(spy.begin(), spy.end(), [&](const auto& q) { return q.ts >= windows[3].first; });
  ASSERT_NE(spy_open, spy.end());
  EXPECT_NEAR(spy_open->last, thursday_open->last / 10.02 - 1.75, 0.011);
  // Each new date opens with the closes the run printed the date before.
  ASSERT_EQ(closes.size(), 4U);
  EXPECT_EQ(closes[0].date, (md::Date{2026, 9, 15}));
  EXPECT_EQ(closes[0].price, 6000);
  EXPECT_EQ(closes[2].ts, windows[2].first);
  EXPECT_EQ(closes[2].symbol, "SPX");
  EXPECT_EQ(closes[2].date, (md::Date{2026, 9, 16}));
  EXPECT_DOUBLE_EQ(closes[2].price, wednesday_close->last);
  const auto spy_close = std::find_if(spy.begin(), spy.end(), [&](const auto& q) { return q.ts >= windows[0].close; });
  ASSERT_NE(spy_close, spy.end());
  EXPECT_EQ(closes[3].symbol, "SPY");
  EXPECT_DOUBLE_EQ(closes[3].price, spy_close->last);
  // Thursday lists its own series around the level it opens at, 4% up.
  EXPECT_TRUE(std::any_of(defined.begin(), defined.end(), [&](const auto& entry) {
    const auto& c = contracts.at(entry.first);
    return entry.second == windows[2].first && c.underlying == "SPX" && c.strike > 6000 * 1.06;
  }));
  // Session volume starts again on the new date.
  std::size_t restarted = 0;
  for (const auto& [id, volume] : thursday_volume)
    if (wednesday_volume.contains(id) && wednesday_volume.at(id) > 0 && volume < wednesday_volume.at(id)) ++restarted;
  EXPECT_GT(restarted, 0U);
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
    if (key.first < at && crush.iv.contains(key)) { EXPECT_DOUBLE_EQ(iv, crush.iv.at(key)); }
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
