#include <fstream>
#include <sstream>
#include <gtest/gtest.h>
#include "openport/server/playbooks.hpp"
#include "openport/server/desk.hpp"
#include "openport/server/api.hpp"
#include "openport/server/run.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/providers/scenario.hpp"
#include "openport/providers/replay_batches.hpp"
#include "openport/trading/history.hpp"
#include "server/strategy_template.hpp"
#include "server/run_json.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;
Money money(std::string_view value) { return Money::parse(value); }
json definition() {
  return {{"id", "morning"}, {"name", "Morning spread"}, {"description", "Synthetic test setup"}, {"underlyings", {"SPX"}},
      {"window", {{"start", "09:30"}, {"end", "11:00"}, {"weekdays", {1, 2, 3, 4, 5}}}}, {"conditions", json::object()},
      {"structure", {{"template", {{"kind", "vertical"}, {"type", "put"}, {"direction", "credit"}, {"target", {{"mode", "delta"}, {"value", 15}}}, {"width", 5}}}, {"expiry", {{"min", 0}, {"max", 60}}}}},
      {"sizing", {{"units", 1}}}, {"management", {{"close_by", "11:30"}, {"take_profit_percent", 50}, {"stop_credit_multiple", 2}}},
      {"guardrails", {{"max_entries_per_day", 1}, {"cooldown_minutes", 30}}}};
}
server::TradingReply command(server::Desk& desk, const json& change, md::Timestamp time) {
  server::TradingCommand request;
  request.kind = server::TradingCommand::Kind::Playbook;
  request.note = change.dump();
  server::TradingReply result;
  desk.command(request, [&](server::TradingReply reply) { result = std::move(reply); }, time, time);
  return result;
}
TEST(Playbooks, DefinitionValidationRejectsUnknownAndInconsistentFields) {
  EXPECT_NO_THROW(server::validate_playbook(definition()));
  for (const auto& [path, value] : std::vector<std::pair<std::string, json>>{
      {"/surprise", 1}, {"/id", "../../x"}, {"/window/start", "25:00"}, {"/window/end", "09:00"},
      {"/window/weekdays", {1, 1}}, {"/sizing/units", 1.5}, {"/sizing/floor_share", .5},
      {"/management/close_by", "09:00"}, {"/conditions/term_inverted", 1}, {"/structure/template/width", 0},
      {"/underlyings", {"SPX", "SPX"}}, {"/guardrails/max_entries_per_day", 0}}) {
    auto bad = definition(); bad[json::json_pointer(path)] = value;
    EXPECT_THROW(server::validate_playbook(bad), std::exception) << path;
  }
}
TEST(Playbooks, EditsVersionAndArchiveRetainsHistoryOnDisk) {
  test::RecordingFile file;
  const auto path = file.directory / "playbooks.json";
  server::Playbooks store(path);
  store.change({{"action", "create"}, {"definition", definition()}}, "main", false);
  auto edited = definition(); edited["version"] = 1; edited["name"] = "Changed";
  store.change({{"action", "update"}, {"definition", edited}}, "main", false);
  EXPECT_THROW(store.change({{"action", "update"}, {"definition", edited}}, "main", false), std::invalid_argument);
  store.change({{"action", "delete"}, {"id", "morning"}, {"version", 2}}, "main", false);
  server::Playbooks recovered(path);
  const auto record = recovered.catalogue().at("definitions").at("morning");
  EXPECT_TRUE(record.at("deleted").get<bool>());
  ASSERT_EQ(record.at("versions").size(), 2U);
  EXPECT_EQ(record.at("versions")[0].at("name"), "Morning spread");
  EXPECT_EQ(record.at("versions")[1].at("version"), 2);
  EXPECT_THROW(recovered.change({{"action", "create"}, {"definition", definition()}}, "main", false), std::invalid_argument);
}
TEST(Playbooks, LivePracticeCannotEnableAutoAndBindingsAreAccountSpecific) {
  server::Playbooks store;
  store.change({{"action", "create"}, {"definition", definition()}}, "main", false);
  const json automatic{{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}};
  EXPECT_THROW(store.change(automatic, "main", false), std::invalid_argument);
  EXPECT_THROW(store.change(automatic, "practice", false), std::invalid_argument);
  EXPECT_NO_THROW(store.change(automatic, "main", true));
  EXPECT_EQ(store.publication("main", true).at("modes").at("morning"), "auto");
  EXPECT_TRUE(store.publication("practice", false).at("modes").empty());
}
TEST(Playbooks, ConditionsFailClosedAndWindowsUseNewYorkWeekdays) {
  auto setup = definition();
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  EXPECT_TRUE(server::playbook_window(setup, time));
  EXPECT_FALSE(server::playbook_window(setup, md::new_york_to_utc({2026, 9, 22}, 11, 0)));
  EXPECT_FALSE(server::playbook_window(setup, md::new_york_to_utc({2026, 9, 26}, 10, 0)));
  setup["conditions"] = {{"price", {{"reference", "prior_close"}, {"direction", "above"}, {"value", 2}}},
      {"iv_rank", {{"min", .3}, {"max", .7}}}, {"vrp_min", 2}, {"term_inverted", true}, {"dte", {{"min", 0}, {"max", 5}}}};
  server::PlaybookInputs inputs{105, 100, 101, .5, 3, 1.2};
  EXPECT_TRUE(server::playbook_conditions(setup, inputs, 1));
  EXPECT_FALSE(server::playbook_conditions(setup, {}, 1));
  inputs.prior_close = 104; EXPECT_FALSE(server::playbook_conditions(setup, inputs, 1));
  inputs.prior_close = 100; inputs.term_ratio = 1; EXPECT_FALSE(server::playbook_conditions(setup, inputs, 1));
  inputs.term_ratio = 1.2; EXPECT_FALSE(server::playbook_conditions(setup, inputs, 6));
  setup["conditions"]["price"]["reference"] = "day_open";
  inputs.day_open = 106; EXPECT_FALSE(server::playbook_conditions(setup, inputs, 1));
  setup["conditions"]["price"] = {{"reference", "level"}, {"direction", "below"}, {"value", 110}};
  EXPECT_TRUE(server::playbook_conditions(setup, inputs, 1));
}
TEST(PlaybookTemplates, MatchesOriginalWebPickerGoldenCases) {
  std::ifstream input(std::filesystem::path(OPENPORT_TEST_DATA_DIR) / "template-parity.json");
  const auto fixtures = json::parse(input);
  std::size_t index = 0;
  for (const auto& test : fixtures.at("cases")) {
    SCOPED_TRACE(index++);
    const auto& near = fixtures.at("chains").at(test.at("near").get<std::size_t>());
    const auto& far = fixtures.at("chains").at(test.at("far").get<std::size_t>());
    if (test.at("result").contains("reason")) {
      // The terminal shows these reasons; they name the contract, strike or range.
      try { (void)server::build_template(test.at("template"), near, far); ADD_FAILURE() << "Built a failing case"; }
      catch (const std::exception& error) { EXPECT_EQ(error.what(), test.at("result").at("reason").get<std::string>()); }
    } else {
      auto actual = server::build_template(test.at("template"), near, far);
      for (auto& leg : actual["legs"]) leg.erase("quote");
      EXPECT_EQ(actual, test.at("result"));
    }
  }
}
TEST(Playbooks, ManagementUsesClosingPremiumAndEarlyCloseDeadline) {
  auto setup = definition(); setup["version"] = 1;
  const auto bracket = server::playbook_bracket(setup.at("management"), money("-2"));
  EXPECT_EQ(bracket.take_profit->limit_price, money("1"));
  EXPECT_EQ(bracket.stop_loss->trigger->level, money("4"));
  EXPECT_THROW(server::playbook_bracket(setup.at("management"), money("2")), std::invalid_argument);
  setup["management"] = {{"close_by", "15:45"}, {"take_profit_percent", 150}, {"stop_underlying", {{"direction", "below"}, {"level", 5900}}}};
  EXPECT_EQ(server::playbook_bracket(setup.at("management"), money("2")).take_profit->limit_price, money("-3"));
  // SPX combos price in nickels: half of a 1.25 credit is 0.625, asked at 0.60, not 0.63.
  trading::OrderRequest spread;
  spread.legs = {{"SPXW  261127P05995000", trading::Side::Sell, 1}, {"SPXW  261127P05990000", trading::Side::Buy, 1}};
  EXPECT_EQ(server::playbook_tick(spread), money("0.05"));
  setup["management"] = {{"close_by", "15:45"}, {"take_profit_percent", 50}};
  EXPECT_EQ(server::playbook_bracket(setup.at("management"), money("-1.25"), server::playbook_tick(spread)).take_profit->limit_price, money("0.60"));
  spread.legs = {{"QQQ   260916P00474000", trading::Side::Sell, 1}, {"QQQ   260916P00469000", trading::Side::Buy, 1}};
  EXPECT_EQ(server::playbook_tick(spread), money("0.01"));
  EXPECT_EQ(server::playbook_bracket(setup.at("management"), money("-1.25"), server::playbook_tick(spread)).take_profit->limit_price, money("0.62"));
  EXPECT_EQ(server::playbook_deadline(setup, md::new_york_to_utc({2026, 11, 27}, 10, 0)), md::new_york_to_utc({2026, 11, 27}, 13, 0));
  EXPECT_EQ(server::playbook_tag(setup), "playbook:morning@v1");
}

std::vector<trading::EvaluationDay> history(Money net, Money low, Money high, bool high_first = false) {
  std::vector<trading::EvaluationDay> days;
  for (int index = 0; index < 10; ++index) {
    trading::EvaluationDay day;
    day.day = md::date_from_days(md::days_since_epoch({2026, 8, 3}) + index);
    day.open_equity = money("1000"); day.close_equity = day.open_equity + net;
    day.low_equity = day.open_equity + low; day.high_equity = day.open_equity + high;
    day.low_at = md::new_york_to_utc(day.day, high_first ? 11 : 10, 0);
    day.high_at = md::new_york_to_utc(day.day, high_first ? 10 : 11, 0);
    days.push_back(day);
  }
  return days;
}
TEST(PassOdds, HandCheckedPassFailAndNeitherUseExactPlanRules) {
  trading::Evaluation current;
  current.starting_balance = current.peak = current.day_close_equity = money("1000");
  current.floor = money("900");
  trading::AccountRules rules; rules.profit_target = money("100"); rules.max_drawdown = money("100");
  auto result = trading::pass_odds(current, rules, money("1000"), history(money("20"), money("-5"), money("20")), 5, 100, 7);
  EXPECT_DOUBLE_EQ(result.pass, 1); EXPECT_EQ(result.median_days_to_pass, 5);
  EXPECT_DOUBLE_EQ(result.fail, 0); EXPECT_DOUBLE_EQ(result.neither, 0);
  result = trading::pass_odds(current, rules, money("1000"), history(money("20"), money("-5"), money("20")), 2, 100, 7);
  EXPECT_DOUBLE_EQ(result.neither, 1);
  result = trading::pass_odds(current, rules, money("1000"), history(money("20"), money("-100"), money("20")), 1, 100, 7);
  EXPECT_DOUBLE_EQ(result.fail, 1); EXPECT_FALSE(result.median_days_to_pass);
  EXPECT_THROW((void)trading::pass_odds(current, rules, money("1000"), {}, 1, 100, 7), std::invalid_argument);
}
TEST(PassOdds, IntradayVersusEndOfDayRatchetAndExtremaOrder) {
  trading::Evaluation current; current.starting_balance = current.peak = money("1000"); current.floor = money("900");
  trading::AccountRules rules; rules.profit_target = money("200"); rules.max_drawdown = money("100");
  const auto days = history(money("10"), money("-60"), money("50"), true);
  EXPECT_DOUBLE_EQ(trading::pass_odds(current, rules, money("1000"), days, 1, 100, 12).fail, 1);
  rules.drawdown_mode = trading::DrawdownMode::EndOfDay;
  EXPECT_DOUBLE_EQ(trading::pass_odds(current, rules, money("1000"), days, 1, 100, 12).neither, 1);
  rules.drawdown_mode = trading::DrawdownMode::Intraday;
  EXPECT_DOUBLE_EQ(trading::pass_odds(current, rules, money("1000"), history(money("10"), money("-60"), money("50")), 1, 100, 12).neither, 1);
  current.floor_locked = true; rules.lock_balance = money("900");
  EXPECT_DOUBLE_EQ(trading::pass_odds(current, rules, money("1000"), days, 1, 100, 12).neither, 1);
}
TEST(PassOdds, TrendingDaysWhoseExtremesMissTheOpenStillCount) {
  trading::Evaluation current; current.starting_balance = current.peak = money("1000"); current.floor = money("900");
  trading::AccountRules rules; rules.profit_target = money("100"); rules.max_drawdown = money("100");
  // Each day gapped up, so its first marked observation, and its low, sit above the open.
  const auto up = trading::pass_odds(current, rules, money("1000"), history(money("20"), money("5"), money("20")), 5, 100, 7);
  EXPECT_EQ(up.historical_days, 10U);
  EXPECT_DOUBLE_EQ(up.pass, 1);
  // Gap-down days whose highs stay under the open still reach the floor through their lows.
  const auto down = trading::pass_odds(current, rules, money("1000"), history(money("-40"), money("-40"), money("-10")), 3, 100, 7);
  EXPECT_DOUBLE_EQ(down.fail, 1);
  EXPECT_DOUBLE_EQ(down.pass + down.fail + down.neither, 1);
}
TEST(PassOdds, SeedIsRepeatableAndOlderMissingExtremaAreNotInvented) {
  trading::Evaluation current; current.starting_balance = current.peak = money("1000"); current.floor = money("900");
  trading::AccountRules rules; rules.profit_target = money("100"); rules.max_drawdown = money("100");
  auto days = history(money("10"), money("-10"), money("20"));
  days[0].close_equity = money("920"); days[0].low_equity = money("900");
  const auto first = trading::pass_odds(current, rules, money("1000"), days, 10, 100, 42);
  const auto second = trading::pass_odds(current, rules, money("1000"), days, 10, 100, 42);
  EXPECT_EQ(first.pass, second.pass); EXPECT_EQ(first.fail, second.fail); EXPECT_EQ(first.seed, 42U);
  days[0].low_equity.reset();
  EXPECT_THROW((void)trading::pass_odds(current, rules, money("1000"), days, 10, 100, 42), std::invalid_argument);
  // Old run commands have no playbook fields and keep their serialized shape.
  server::TradingCommand request;
  const auto restored = json(request).get<server::TradingCommand>();
  EXPECT_EQ(restored.kind, server::TradingCommand::Kind::Submit);
}
struct ScenarioFixture {
  test::RecordingFile file;
  providers::Scenario scenario;
  explicit ScenarioFixture(bool multiple_symbols = false)
      : ScenarioFixture(multiple_symbols ? std::vector<std::string>{"SPX", "SPY"} : std::vector<std::string>{"SPX"}) {}
  explicit ScenarioFixture(const std::vector<std::string>& symbols) {
    const auto path = file.directory / "playbook-scenario.json";
    { std::ofstream out(path); out << R"({"id":"playbook-test","title":"Playbook test","description":"Simulated test day","symbols":["SPX"],"session":"regular","date":"2026-11-27","seed":81723,"generator":1,"drift":[[1,0.001]],"volatility":0.12,"iv_shift":0,"spot_vol":-2})"; }
    if (symbols != std::vector<std::string>{"SPX"}) {
      std::ifstream input(path); auto document = json::parse(input); input.close();
      document["symbols"] = symbols;
      std::ofstream output(path); output << document.dump();
    }
    scenario = providers::read_scenario(path);
    providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  }
  json setup() const {
    auto value = definition();
    value["window"]["end"] = "09:31";
    value["management"] = {{"close_by", "09:32"}};
    value["structure"]["template"]["target"] = {{"mode", "delta"}, {"value", 40}};
    value["structure"]["expiry"]["max"] = 1;
    return value;
  }
};
TEST(Playbooks, StagingUsesPreviewWithoutJournalOrdersAndExpiresOrDismisses) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  desk.replay_batch(first->events, first->received, first->time);
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", fixture.setup()}}, desk.market_time()).decision.ok());
  const auto count = desk.trading_view()->snapshot->account_version;
  auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, desk.market_time());
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  auto publication = json::parse(reply.playbook_result);
  ASSERT_EQ(publication.at("staged").size(), 1U) << publication.dump();
  EXPECT_EQ(desk.trading_view()->snapshot->account_version, count);
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_orders.empty());
  const auto stage = publication.at("staged")[0];
  EXPECT_EQ(stage.at("units"), 1);
  EXPECT_FALSE(stage.at("max_loss").is_null());
  reply = command(desk, {{"action", "dismiss"}, {"staged", stage.at("id")}}, desk.market_time());
  EXPECT_TRUE(json::parse(reply.playbook_result).at("staged").empty());
  reply = command(desk, {{"action", "send"}, {"staged", stage.at("id")}}, desk.market_time());
  EXPECT_FALSE(reply.decision.ok());
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_orders.empty());
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.market_time() >= md::new_york_to_utc(fixture.scenario.date, 9, 31)) break;
  }
  EXPECT_TRUE(json::parse(desk.trading_view()->playbooks_json).at("staged").empty());
}
TEST(Playbooks, AStageKeepsItsIdWhileQuotesMoveAndSendsTheLatestOrder) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  desk.replay_batch(first->events, first->received, first->time);
  auto setup = fixture.setup();
  setup["window"]["end"] = "09:45";
  // SPX exits: the take-profit must land on a nickel for the combo to be accepted.
  setup["management"] = {{"close_by", "09:50"}, {"take_profit_percent", 50}, {"stop_credit_multiple", 2}};
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time()).decision.ok());
  auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, desk.market_time());
  ASSERT_EQ(json::parse(reply.playbook_result).at("staged").size(), 1U) << reply.playbook_result;
  auto staged = json::parse(reply.playbook_result).at("staged")[0];
  // The market moves on before the trader clicks Send. While the same contracts are
  // selected the stage keeps its ID; other contracts are a different stage.
  const auto contracts = [](const json& stage) {
    std::string symbols;
    for (const auto& leg : stage.at("legs")) symbols += leg.at("symbol").get<std::string>() + ";";
    return symbols;
  };
  for (bool repriced = false; !repriced;) {
    const auto batch = batches.next(); ASSERT_TRUE(batch);
    desk.replay_batch(batch->events, batch->received, batch->time);
    ASSERT_LT(desk.market_time(), md::new_york_to_utc(fixture.scenario.date, 9, 45));
    const auto stages = json::parse(desk.trading_view()->playbooks_json).at("staged");
    ASSERT_EQ(stages.size(), 1U) << json::parse(desk.trading_view()->playbooks_json).at("reasons").dump();
    const bool same = contracts(stages[0]) == contracts(staged);
    EXPECT_EQ(stages[0].at("id") == staged.at("id"), same);
    repriced = same && stages[0].at("time") != staged.at("time");
    if (!same) staged = stages[0];  // What the trader now sees.
  }
  server::TradingCommand send;
  send.kind = server::TradingCommand::Kind::Playbook;
  send.actor = "alice";
  send.note = json{{"action", "send"}, {"staged", staged.at("id")}}.dump();
  desk.command(send, [&](server::TradingReply result) { reply = std::move(result); }, desk.market_time(), desk.market_time());
  ASSERT_TRUE(reply.decision.ok() && reply.error_code.empty()) << reply.decision.message;
  const auto& orders = desk.trading_view()->snapshot->recent_orders;
  ASSERT_FALSE(orders.empty());
  EXPECT_TRUE(orders[0].request.client_order_id.starts_with("playbook:" + staged.at("id").get<std::string>() + ":"));
  EXPECT_EQ(orders[0].actor, "alice");
  ASSERT_TRUE(orders[0].request.bracket && orders[0].request.bracket->take_profit);
  EXPECT_EQ(orders[0].request.bracket->take_profit->limit_price->micros() % 50'000, 0);
}
TEST(Playbooks, ScenarioAutoEntriesAndTimeStopsReproduceJournalAndVerification) {
  ScenarioFixture fixture;
  server::Playbooks store;
  auto setup = fixture.setup();
  setup["conditions"]["price"] = {{"reference", "day_open"}, {"direction", "above"}, {"value", -1000}};
  setup["sizing"] = {{"floor_share", .8}};
  store.change({{"action", "create"}, {"definition", setup}}, "main", true);
  store.change({{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, "main", true);
  std::string golden;
  for (int repeat = 0; repeat < 2; ++repeat) {
    md::RecordingReader reader(fixture.file.path);
    const auto journal = fixture.file.directory / ("auto-" + std::to_string(repeat) + ".jsonl");
    server::Desk::Options options; options.replay = true;
    options.paper_journal = journal;
    options.initial_playbooks = store.catalogue().dump();
    options.paper.initial_cash = money("50000");
    options.paper.guardrails.soft_floor = money("49000");
    options.candles = std::make_shared<server::CandleStore>();
    options.run_input = server::scenario_input(fixture.scenario, fixture.scenario.date, fixture.scenario.seed);
    options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    while (const auto batch = batches.next()) {
      desk.replay_batch(batch->events, batch->received, batch->time);
      if (desk.market_time() >= md::new_york_to_utc(fixture.scenario.date, 9, 33)) break;
    }
    const auto view = desk.trading_view();
    ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
    ASSERT_GE(view->snapshot->recent_fills.size(), 4U) << view->playbooks_json;
    EXPECT_TRUE(view->snapshot->positions.empty());
    EXPECT_TRUE(view->snapshot->open_orders.empty());
    EXPECT_EQ(view->snapshot->recent_orders.front().request.tags.front(), "playbook:morning@v1");
    // Automatic entries and time stops are the system's actions.
    for (const auto& order : view->snapshot->recent_orders) {
      EXPECT_EQ(order.actor, "system") << order.request.client_order_id;
    }
    const auto report = server::playbook_report(store.catalogue(), *view).at("morning");
    EXPECT_EQ(report.at("all").at("trades"), 1);
    EXPECT_EQ(report.at("all").at("adherence"), 1);
    EXPECT_EQ(report.at("trades")[0].at("rules").at("time_stop"), true);
    auto malformed = *view;
    auto snapshot = std::make_shared<trading::TradingSnapshot>(*view->snapshot);
    snapshot->recent_orders.mut(0).request.note = R"({"playbook":42,"max_units":1,"floor_share":0.8})";
    malformed.snapshot = snapshot;
    EXPECT_NO_THROW(server::playbook_report(store.catalogue(), malformed));
    std::ifstream input(journal); std::ostringstream bytes; bytes << input.rdbuf();
    if (repeat == 0) { golden = bytes.str(); } else { EXPECT_EQ(bytes.str(), golden); }
    desk.stop();
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}
/// Each contract's quote in a recording as of `time`: its latest quote then.
std::map<std::string, std::pair<Money, Money>> quotes_at(const std::filesystem::path& file, md::Timestamp time) {
  md::RecordingReader reader(file);
  std::map<md::InstrumentId, std::string> symbols;
  std::map<std::string, std::pair<Money, Money>> quotes;
  while (const auto record = reader.next()) {
    if (const auto* definition = std::get_if<md::ContractDefinition>(&record->event))
      symbols[definition->id] = definition->contract.osi_symbol();
    else if (const auto* quote = std::get_if<md::OptionQuote>(&record->event); quote && quote->ts <= time)
      quotes[symbols.at(quote->id)] = {Money::from_double(quote->bid), Money::from_double(quote->ask)};
  }
  return quotes;
}
TEST(Playbooks, AutomaticEntriesTradeTheBookOfTheirOwnMarketInstant) {
  // B46: a replay writes QQQ's snapshot of each market time before SPX's. QQQ's
  // analytics at 09:45:00 started the playbook, whose SPX entry traded SPX's 09:44:45
  // book, stamped 09:45:00. Backtests replay days the same way.
  ScenarioFixture fixture(std::vector<std::string>{"QQQ", "SPX", "SPY"});
  auto setup = fixture.setup();
  setup["window"] = {{"start", "09:45"}, {"end", "10:00"}, {"weekdays", {1, 2, 3, 4, 5}}};
  setup["management"] = {{"close_by", "10:30"}};
  server::Playbooks store;
  store.change({{"action", "create"}, {"definition", setup}}, "main", true);
  store.change({{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, "main", true);
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.initial_playbooks = store.catalogue().dump();
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto entry = md::new_york_to_utc(fixture.scenario.date, 9, 45);
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.market_time() >= entry) break;
  }
  const auto view = desk.trading_view();
  ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
  const auto fills = view->snapshot->recent_fills.to_vector();
  ASSERT_EQ(fills.size(), 2U) << view->playbooks_json;
  const auto book = quotes_at(fixture.file.path, entry);
  for (const auto& fill : fills) {
    EXPECT_EQ(fill.time, entry);
    EXPECT_EQ(fill.quote_time, entry);
    const auto& [bid, ask] = book.at(fill.symbol);
    EXPECT_EQ(fill.price, fill.side == trading::Side::Buy ? ask : bid) << fill.symbol;
  }
}
TEST(Playbooks, ExpectancyAndRuleSplitsUseWholeStrategies) {
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  trading::TradingSession session(config, market.time);
  auto setup = definition(); setup["management"] = {{"close_by", "11:30"}};
  setup["guardrails"]["max_entries_per_day"] = 10;
  setup["guardrails"]["cooldown_minutes"] = 0;
  server::Playbooks store;
  store.change({{"action", "create"}, {"definition", setup}}, "main", false);
  for (int index = 0; index < 2; ++index) {
    market.seed(session);
    auto entry = market.market("entry" + std::to_string(index), index + 1);
    entry.tags = {"playbook:morning@v1"};
    ASSERT_TRUE(session.submit(entry, market.time).decision.ok());
    market.next();
    session.on_quotes({market.quote(index == 0 ? "5.20" : "3.20", index == 0 ? "5.40" : "3.40")}, {market.valuation()}, market.time);
    ASSERT_TRUE(session.submit(market.market("close" + std::to_string(index), index + 1, trading::Side::Sell), market.time).decision.ok());
    market.next();
  }
  server::TradingView view; view.snapshot = session.snapshot(); view.contracts = session.contracts(); view.config = config;
  const auto report = server::playbook_report(store.catalogue(), view).at("morning");
  EXPECT_EQ(report.at("all").at("trades"), 2);
  EXPECT_DOUBLE_EQ(report.at("all").at("win_rate").get<double>(), .5);
  EXPECT_EQ(report.at("followed").at("trades"), 1);
  EXPECT_EQ(report.at("deviated").at("trades"), 1);
  const auto first = Money::parse(report.at("trades")[0].at("net").get<std::string>());
  const auto second = Money::parse(report.at("trades")[1].at("net").get<std::string>());
  EXPECT_EQ(report.at("all").at("expectancy"), (first + second).prorate(1, 2).str());
  EXPECT_DOUBLE_EQ(report.at("all").at("profit_factor").get<double>(), first.dollars() / -second.dollars());
  EXPECT_TRUE(report.at("all").at("average_r").is_null());
}
class DeskSource final : public server::MetricsSource {
 public:
  explicit DeskSource(server::Desk& desk) : desk_(desk) {}
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string& symbol) const override { return desk_.metrics(symbol); }
  server::EngineStatus status() const override { server::EngineStatus value; value.trading = desk_.trading_status(); value.accounts = desk_.accounts(); return value; }
  using MetricsSource::trading_view;
  std::shared_ptr<const server::TradingView> trading_view() const override { return desk_.trading_view(); }
  md::Timestamp wall_time() const override { return desk_.market_time(); }
  bool post_trading(server::TradingCommand request, server::TradingCompletion complete) override {
    desk_.command(std::move(request), std::move(complete), desk_.market_time(), desk_.market_time()); return true;
  }
 private:
  server::Desk& desk_;
};
server::ApiResponse api(DeskSource& source, std::string method, std::string route, json body = nullptr) {
  server::ApiResponse reply;
  server::handle_api_async({std::move(method), std::move(route), body.is_null() ? "" : body.dump()}, source,
                          [&](server::ApiResponse value) { reply = std::move(value); });
  return reply;
}
TEST(PlaybookApi, VersionReadsAndAutoRefusalUseAccountCommandPath) {
  server::Desk desk("test", {}, {{"SPX"}}, {}); desk.start_trading();
  DeskSource source(desk);
  ASSERT_EQ(api(source, "POST", "/api/playbooks", definition()).status, 200);
  auto edit = definition(); edit["version"] = 1; edit["name"] = "Version two";
  ASSERT_EQ(api(source, "PUT", "/api/playbooks/morning", edit).status, 200);
  EXPECT_EQ(json::parse(api(source, "GET", "/api/playbooks/morning?version=1").body).at("name"), "Morning spread");
  EXPECT_EQ(json::parse(api(source, "GET", "/api/playbooks/morning").body).at("name"), "Version two");
  EXPECT_EQ(api(source, "PUT", "/api/playbooks/morning/mode", {{"mode", "auto"}}).status, 400);
  EXPECT_EQ(api(source, "GET", "/api/playbooks?version=1&version=2").status, 400);
  EXPECT_EQ(api(source, "GET", "/api/playbooks/morning?version=99").status, 404);
  EXPECT_EQ(api(source, "DELETE", "/api/playbooks/morning?version=1").status, 400);
  EXPECT_EQ(api(source, "DELETE", "/api/playbooks/morning?version=2").status, 200);
  EXPECT_EQ(api(source, "GET", "/api/playbooks/morning?version=1").status, 200);
  EXPECT_TRUE(desk.trading_status().enabled);
}
TEST(Playbooks, FloorShareAndFixedSizesUseTheNormalPreviewAndLiveSendPath) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options;
  options.paper.initial_cash = money("50000"); options.paper.rules.max_drawdown = money("1000");
  options.paper.limits.max_order_contracts = 2;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("test", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  desk.replay_batch(first->events, first->received, first->time);
  auto setup = fixture.setup(); setup["sizing"] = {{"floor_share", .8}};
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time()).decision.ok());
  auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, desk.market_time());
  auto stages = json::parse(reply.playbook_result).at("staged");
  ASSERT_EQ(stages.size(), 1U) << reply.playbook_result;
  const auto request = stages[0].at("request").get<trading::OrderRequest>();
  server::TradingCommand preview; preview.kind = server::TradingCommand::Kind::Preview; preview.order = request; preview.floor_share = .8;
  server::TradingReply projection;
  desk.command(preview, [&](server::TradingReply result) { projection = std::move(result); }, desk.market_time(), desk.market_time());
  ASSERT_TRUE(projection.preview);
  EXPECT_EQ(request.quantity, projection.preview->max_units);
  EXPECT_LE(request.quantity, 2);
  EXPECT_LE(*projection.preview->max_loss, money("800"));
  setup["version"] = 1; setup["sizing"] = {{"units", 100}};
  reply = command(desk, {{"action", "update"}, {"definition", setup}}, desk.market_time());
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  stages = json::parse(reply.playbook_result).at("staged");
  ASSERT_EQ(stages.size(), 1U);
  EXPECT_LE(stages[0].at("units").get<int>(), 2);
  const auto stage_id = stages[0].at("id");
  reply = command(desk, {{"action", "send"}, {"staged", stage_id}}, desk.market_time());
  EXPECT_TRUE(reply.decision.ok()) << reply.decision.message;
  ASSERT_FALSE(desk.trading_view()->snapshot->recent_fills.empty());
  const auto count = desk.trading_view()->snapshot->recent_orders.size();
  reply = command(desk, {{"action", "send"}, {"staged", stage_id}}, desk.market_time());
  EXPECT_FALSE(reply.decision.ok());
  EXPECT_EQ(desk.trading_view()->snapshot->recent_orders.size(), count);
}
TEST(Playbooks, ConditionsExpireStagesAndFloorSizingRequiresAnActualFloor) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  server::Desk desk("test", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading(); providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first); desk.replay_batch(first->events, first->received, first->time);
  auto setup = fixture.setup();
  command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time());
  auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, desk.market_time());
  ASSERT_FALSE(json::parse(reply.playbook_result).at("staged").empty());
  setup["version"] = 1; setup["conditions"]["price"] = {{"reference", "level"}, {"direction", "above"}, {"value", 100000}};
  reply = command(desk, {{"action", "update"}, {"definition", setup}}, desk.market_time());
  EXPECT_TRUE(json::parse(reply.playbook_result).at("staged").empty());
  setup["version"] = 2; setup["conditions"] = json::object(); setup["sizing"] = {{"floor_share", .5}};
  reply = command(desk, {{"action", "update"}, {"definition", setup}}, desk.market_time());
  EXPECT_TRUE(json::parse(reply.playbook_result).at("staged").empty());
  EXPECT_NE(reply.playbook_result.find("needs a plan or personal floor"), std::string::npos);
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_orders.empty());
}
TEST(Playbooks, AdherenceDetectsWindowSizeExitTimeAndLossCooldownDeviations) {
  test::ScriptedMarket market; market.time = md::new_york_to_utc({2026, 9, 22}, 9, 35);
  trading::SessionConfig config;
  config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  trading::TradingSession session(config, market.time);
  auto setup = definition(); setup["window"]["start"] = "09:36"; setup["guardrails"]["max_entries_per_day"] = 10;
  server::Playbooks store; store.change({{"action", "create"}, {"definition", setup}}, "main", false);
  market.seed(session);
  auto entry = market.market("first"); entry.tags = {"playbook:morning@v1"};
  ASSERT_TRUE(session.submit(entry, market.time).decision.ok());
  market.next(); session.on_quotes({market.quote("3.20", "3.40")}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("close-first", 1, trading::Side::Sell), market.time).decision.ok());
  market.time = md::new_york_to_utc({2026, 9, 22}, 9, 36); ++market.observation; market.seed(session);
  entry = market.market("second", 2); entry.tags = {"playbook:morning@v1"};
  ASSERT_TRUE(session.submit(entry, market.time).decision.ok());
  market.time = md::new_york_to_utc({2026, 9, 22}, 12, 0); ++market.observation;
  session.on_quotes({market.quote()}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("close-second", 2, trading::Side::Sell), market.time).decision.ok());
  server::TradingView view; view.snapshot = session.snapshot(); view.config = config; view.contracts = session.contracts();
  const auto report = server::playbook_report(store.catalogue(), view).at("morning");
  EXPECT_FALSE(report.at("trades")[0].at("rules").at("entry_window").get<bool>());
  const auto rules = report.at("trades")[1].at("rules");
  EXPECT_TRUE(rules.at("entry_window").get<bool>());
  for (const auto* rule : {"size", "exits", "time_stop", "guardrails"}) { EXPECT_FALSE(rules.at(rule).get<bool>()) << rule; }
}
TEST(Playbooks, OlderRunWithoutCatalogueStillVerifiesAndArchivesUseRecordedVersions) {
  ScenarioFixture fixture;
  const auto directory = fixture.file.directory / "replays";
  std::filesystem::create_directories(directory);
  for (const bool legacy : {true, false}) {
    md::RecordingReader reader(fixture.file.path);
    const auto id = legacy ? "legacy" : "versions";
    const auto file = directory / (std::string(id) + ".jsonl");
    server::Desk::Options options; options.replay = true; options.paper_journal = file;
    options.run_input = server::scenario_input(fixture.scenario, fixture.scenario.date, fixture.scenario.seed);
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading(); providers::ReplayBatches batches(reader, reader.header().subscription);
    const auto first = batches.next(); ASSERT_TRUE(first); desk.replay_batch(first->events, first->received, first->time);
    if (!legacy) {
      auto setup = fixture.setup();
      ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time()).decision.ok());
      setup["version"] = 1; setup["name"] = "Later version";
      ASSERT_TRUE(command(desk, {{"action", "update"}, {"definition", setup}}, desk.market_time()).decision.ok());
    }
    desk.stop();
    const auto verified = server::verify_run(file); EXPECT_TRUE(verified.matched) << verified.message;
    const auto recovery = trading::FileJournal::read(file.string());
    bool checked = false;
    for (const auto& record : recovery.records) if (record.type == "run_input") {
      const auto payload = json::parse(record.payload);
      for (const auto& event : payload.at("events")) if (event.at("type") == "run_input" && event.at("payload").at("kind") == "start") {
        EXPECT_FALSE(event.at("payload").contains("playbooks")); checked = true;
      }
    }
    EXPECT_TRUE(checked);
  }
  server::ReplayHost::Options options; options.demo = false; options.engine.paper_journal = fixture.file.directory / "main.jsonl";
  server::ReplayHost host(options);
  server::ApiResponse reply;
  ASSERT_TRUE(host.handle({"GET", "/api/replay/history/versions/playbooks/morning?version=1"}, [&](server::ApiResponse result) { reply = std::move(result); }));
  ASSERT_EQ(reply.status, 200) << reply.body;
  EXPECT_EQ(json::parse(reply.body).at("name"), "Morning spread");
  ASSERT_TRUE(host.handle({"GET", "/api/replay/history/versions/playbooks/morning"}, [&](server::ApiResponse result) { reply = std::move(result); }));
  EXPECT_EQ(json::parse(reply.body).at("name"), "Later version");
}
class OddsSource final : public server::MetricsSource {
 public:
  std::shared_ptr<server::TradingView> view = std::make_shared<server::TradingView>();
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  server::EngineStatus status() const override { return {}; }
  using MetricsSource::trading_view;
  std::shared_ptr<const server::TradingView> trading_view() const override { return view; }
};
TEST(PlaybookApi, PassOddsReturnsSeedLabelsAndClearHistoryAndParameterErrors) {
  OddsSource source;
  auto snapshot = std::make_shared<trading::TradingSnapshot>(); snapshot->equity = money("1000"); snapshot->valuation_complete = true;
  snapshot->evaluation.starting_balance = snapshot->evaluation.peak = money("1000"); snapshot->evaluation.floor = money("900");
  snapshot->evaluation.days = history(money("20"), money("-5"), money("20"));
  source.view->snapshot = snapshot; source.view->config.rules.profit_target = money("100"); source.view->config.rules.max_drawdown = money("100");
  const auto response = server::handle_api({"GET", "/api/account/pass-odds?days=5&samples=100&seed=42"}, source);
  ASSERT_EQ(response.status, 200) << response.body;
  const auto result = json::parse(response.body);
  EXPECT_EQ(result.at("seed"), "42"); EXPECT_EQ(result.at("historical_days"), 10); EXPECT_EQ(result.at("pass"), 1);
  EXPECT_EQ(result.at("label"), "Estimate from past results, not a prediction");
  EXPECT_EQ(server::handle_api({"GET", "/api/account/pass-odds?days=0"}, source).status, 400);
  EXPECT_EQ(server::handle_api({"GET", "/api/account/pass-odds?seed=42&seed=42"}, source).status, 400);
  EXPECT_EQ(server::handle_api({"GET", "/api/account/pass-odds?playbook=unknown"}, source).status, 404);
  snapshot->evaluation.days.pop_back();
  const auto short_history = server::handle_api({"GET", "/api/account/pass-odds"}, source);
  EXPECT_EQ(short_history.status, 400); EXPECT_NE(short_history.body.find("at least 10 completed days"), std::string::npos);
}
TEST(Playbooks, AutomaticEntryLimitIsSharedAcrossUnderlyingsWithinOneUpdate) {
  ScenarioFixture fixture(true);
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading(); providers::ReplayBatches batches(reader, reader.header().subscription);
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.metrics("SPX") && desk.metrics("SPY")) break;
  }
  auto setup = fixture.setup(); setup["underlyings"] = {"SPX", "SPY"};
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time()).decision.ok());
  const auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, desk.market_time());
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  ASSERT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 1U) << reply.playbook_result;
  EXPECT_EQ(desk.trading_view()->snapshot->recent_orders.front().request.tags.front(), "playbook:morning@v1");
  EXPECT_EQ(json::parse(reply.playbook_result).at("reasons").at("morning:SPY"), "Playbook entry limit or loss cooldown");
}
TEST(Playbooks, AnAutomaticOrderLeavesItsEvaluationPublished) {
  // B43: an automatic order publishes the account mid-evaluation. That publication,
  // cached by revision, hid the evaluation's own reasons and stages.
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading(); providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first); desk.replay_batch(first->events, first->received, first->time);
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", fixture.setup()}}, desk.market_time()).decision.ok());
  auto calls = fixture.setup(); calls["id"] = "staged"; calls["structure"]["template"]["type"] = "call";
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", calls}}, desk.market_time()).decision.ok());
  ASSERT_TRUE(command(desk, {{"action", "mode"}, {"id", "staged"}, {"mode", "stage"}}, desk.market_time()).decision.ok());
  const auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, desk.market_time());
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  ASSERT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 1U) << reply.playbook_result;
  const auto published = json::parse(desk.trading_view()->playbooks_json);
  EXPECT_EQ(published.at("reasons").at("morning:SPX"), "Automatic order sent") << published.dump();
  EXPECT_EQ(published.at("reasons").at("staged:SPX"), "Ready");
  ASSERT_EQ(published.at("staged").size(), 1U);
  EXPECT_EQ(published.at("staged"), json::parse(reply.playbook_result).at("staged"));
}
/// A 0DTE SPX put spread's day, one snapshot a minute from 09:30. The 4995 wing has no
/// bid from 09:32 until `bid_back` minutes after the open.
std::filesystem::path no_bid_wing_day(const std::filesystem::path& directory, int bid_back) {
  const md::Date date{2026, 9, 16};
  const auto path = directory / "no-bid-wing.oprec";
  auto contract = *md::parse_osi("SPXW  260916P05000000");
  auto lower = contract; lower.strike = 4995;
  md::RecordingHeader header;
  header.provider = "simulated test";
  header.subscription.underlyings = {"SPX"};
  header.started = md::new_york_to_utc(date, 9, 30);
  header.capabilities.poll_interval = std::chrono::seconds(60);
  md::Timestamp time = header.started;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return time; };
  test::DiscardEvents discard;
  md::RecordingSink sink(path, header, discard, recording);
  sink.publish(md::ContractDefinition{0, contract});
  sink.publish(md::ContractDefinition{1, lower});
  for (int minute = 0; minute <= bid_back + 1; ++minute) {
    time = header.started + minute * md::kNanosPerMinute;
    const bool bid = minute < 2 || minute >= bid_back;
    sink.publish(md::UnderlyingQuote{"SPX", time, 5000, 5000, 5000});
    sink.publish(md::OptionQuote{0, time, 10, 10.1, 20, 20});
    sink.publish(md::OptionQuote{1, time, bid ? 8.0 : 0.0, 8.1, bid ? 20.0 : 0.0, 20});
    sink.publish(md::SnapshotComplete{"SPX", time});
  }
  sink.close();
  return path;
}
TEST(Playbooks, ATimeStopWaitsForLiquidityWithoutJournalingRejections) {
  // B44: while a wing had no bid, the time stop's combo close was submitted and
  // rejected at every update, piling up rejected orders in the journal.
  test::RecordingFile file;
  const auto recording = no_bid_wing_day(file.directory, 6);
  auto setup = definition();
  setup["window"]["end"] = "09:31";
  setup["structure"]["template"]["target"] = {{"mode", "strike"}, {"value", 5000}};
  setup["structure"]["expiry"]["max"] = 1;
  setup["management"] = {{"close_by", "09:32"}, {"take_profit_percent", 50}};
  server::Playbooks store;
  store.change({{"action", "create"}, {"definition", setup}}, "main", true);
  store.change({{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, "main", true);
  md::RecordingReader reader(recording);
  server::Desk::Options options; options.replay = true;
  options.initial_playbooks = store.catalogue().dump();
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (test)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto count = [&](auto predicate) {
    const auto orders = desk.trading_view()->snapshot->recent_orders.to_vector();
    return std::count_if(orders.begin(), orders.end(), predicate);
  };
  const auto closes = [](const trading::Order& order) { return order.request.client_order_id.starts_with("pb-close:"); };
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
    if (desk.market_time() < md::new_york_to_utc({2026, 9, 16}, 9, 36)) EXPECT_EQ(count(closes), 0);
  }
  const auto view = desk.trading_view();
  EXPECT_EQ(count([](const trading::Order& order) { return order.status == trading::OrderStatus::Rejected; }), 0);
  ASSERT_EQ(count(closes), 1);
  // The exits were cancelled once, at the deadline, and the close filled when the wing was bid.
  for (const auto& order : view->snapshot->recent_orders) {
    if (order.parent) { EXPECT_EQ(order.status, trading::OrderStatus::Cancelled); }
    if (closes(order)) {
      EXPECT_EQ(order.status, trading::OrderStatus::Filled);
      EXPECT_EQ(order.accepted_at, md::new_york_to_utc({2026, 9, 16}, 9, 36));
    }
  }
  EXPECT_TRUE(view->snapshot->positions.empty());
}
}  // namespace
