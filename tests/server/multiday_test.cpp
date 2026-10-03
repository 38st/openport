#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <map>
#include <optional>

#include "openport/providers/replay_batches.hpp"
#include "openport/providers/scenario.hpp"
#include "openport/server/desk.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/run.hpp"
#include "support/recording.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;

/// Wednesday's regular and curb sessions, then Thursday's overnight session, in a quiet
/// market: SPY goes ex-dividend on Thursday.
constexpr const char* kCarry = R"({"id":"carry","title":"Carry","description":"A day, its curb and the night after.",
  "symbols":["SPX","SPY"],"date":"2026-09-16","seed":5,"generator":1,"volatility":0.02,"iv_shift":0,"spot_vol":-2,
  "sessions":[{"session":"regular","drift":[[1,0]]},{"session":"curb","drift":[[1,0]]},
    {"session":"overnight","drift":[[1,0]],"dividends":[{"symbol":"SPY","per_share":1.75}]}]})";

/// The Friday after Thanksgiving closes early; Sunday evening's overnight session leads
/// into Monday's regular session. SPX alone keeps the run quick.
constexpr const char* kWeekend = R"({"id":"weekend","title":"Weekend","description":"Friday, the night into Monday, and Monday.",
  "symbols":["SPX"],"date":"2026-11-27","seed":7,"generator":1,"volatility":0.05,"iv_shift":0,"spot_vol":-2,
  "sessions":[{"session":"regular","drift":[[1,0.002]]},{"session":"overnight","drift":[[1,-0.001]]},
    {"session":"regular","drift":[[1,0.001]],"events":[{"type":"gap","move":0.002}]}]})";

server::ApiResponse call(server::ReplayHost& host, std::string method, std::string target, const json& body = json::object()) {
  std::promise<server::ApiResponse> done;
  auto result = done.get_future();
  server::ApiRequest request{std::move(method), std::move(target), body.dump()};
  request.content_type = "application/json";
  request.actor = "test-actor";
  if (!host.handle(request, [&](server::ApiResponse response) { done.set_value(std::move(response)); }))
    throw std::runtime_error("Replay request was not handled");
  if (result.wait_for(std::chrono::minutes(5)) != std::future_status::ready) throw std::runtime_error("Replay request timed out");
  return result.get();
}
server::TradingReply command(server::Desk& desk, server::TradingCommand request, md::Timestamp time, md::Timestamp driver) {
  std::optional<server::TradingReply> result;
  desk.command(std::move(request), [&](server::TradingReply reply) { result = std::move(reply); }, time, driver);
  if (!result) throw std::runtime_error("Desk did not complete command");
  return *result;
}
server::TradingCommand order(std::string id, const char* osi, trading::Side side, trading::Quantity quantity,
                             trading::OrderType type, trading::TimeInForce tif, std::optional<Money> limit = {}) {
  server::TradingCommand request;
  request.actor = "trader";
  request.order.client_order_id = std::move(id);
  request.order.symbol = md::parse_osi(osi)->osi_symbol();
  request.order.side = side;
  request.order.quantity = quantity;
  request.order.type = type;
  request.order.tif = tif;
  request.order.limit_price = limit;
  return request;
}
const trading::Order* find(const trading::TradingSnapshot& snapshot, const std::string& id) {
  for (const auto& o : snapshot.recent_orders) if (o.request.client_order_id == id) return &o;
  return nullptr;
}

TEST(MultiDayReplay, OneRunCarriesTheAccountThroughEverySessionAndItsJournalVerifies) {
  test::RecordingFile file;
  const auto source = file.directory / "carry.json";
  { std::ofstream out(source); out << kCarry; }
  const auto scenario = providers::read_scenario(source);
  const auto windows = providers::scenario_windows(scenario, scenario.date);
  ASSERT_EQ(windows.size(), 3U);
  providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  const auto journal = file.directory / "run.jsonl";
  const auto* plan = server::find_plan("eod-100k");
  ASSERT_TRUE(plan);
  const md::Date wednesday{2026, 9, 16}, thursday{2026, 9, 17};
  const auto at = [](md::Date date, int hour, int minute) { return md::new_york_to_utc(date, hour, minute); };
  std::map<std::string, server::TradingReply> replies;
  {
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.replay = true;
    options.run_input = server::scenario_input(scenario, scenario.date, scenario.seed);
    options.paper_journal = journal;
    options.paper.rules = plan->rules;
    options.paper.initial_cash = plan->initial_cash;
    options.dividends = {{"SPY", thursday, Money::parse("1.75")}};
    // European analytics are enough here; the American pass doubles the run's time.
    options.analytics.deamericanize = false;
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    const auto send = [&](md::Timestamp time, md::Timestamp receipt, server::TradingCommand request) {
      replies[request.order.client_order_id] = command(desk, request, time, receipt);
    };
    // Resting limits inside the price band, away from the touch: SPX's ticks are $0.10 from $3.
    std::map<std::string, md::InstrumentId> ids;
    const auto away = [&](const char* osi, double factor) {
      const auto* option = desk.book().option(ids.at(md::parse_osi(osi)->osi_symbol()));
      const double price = option->mid() * factor;
      return Money::from_double((factor < 1 ? std::floor(price * 10) : std::ceil(price * 10)) / 10);
    };
    using enum trading::Side;
    using enum trading::OrderType;
    using enum trading::TimeInForce;
    // The run stops a few minutes into Thursday's overnight session: a stopped run
    // verifies through what it played.
    const auto stop = at(wednesday, 21, 1);
    while (const auto batch = batches.next()) {
      if (batch->time > stop) break;
      for (const auto& event : batch->events)
        if (const auto* definition = std::get_if<md::ContractDefinition>(&event)) ids[definition->contract.osi_symbol()] = definition->id;
      desk.replay_batch(batch->events, batch->received, batch->time);
      const auto time = desk.market_time();
      if (time == at(wednesday, 10, 0)) {
        send(time, batch->received, order("held", "SPXW260918C06000000", Buy, 1, Market, Ioc));
        // GTC orders carry from day to day; a DAY order ends with its session.
        send(time, batch->received, order("gtc", "SPX261016P05800000", Buy, 1, Limit, Gtc, away("SPX261016P05800000", 0.82)));
        send(time, batch->received, order("day", "SPX261016P05800000", Buy, 1, Limit, Day, away("SPX261016P05800000", 0.82)));
        // Calls with less time value than tomorrow's dividend can be assigned overnight.
        send(time, batch->received, order("short", "SPY260925C00580000", Sell, 5, Market, Ioc));
      } else if (time == at(wednesday, 16, 30)) {
        // The curb takes plain limit orders, a DAY one lasting until 17:00.
        send(time, batch->received, order("curb-market", "SPXW260918C06000000", Sell, 1, Market, Ioc));
        send(time, batch->received, order("curb-limit", "SPX261016P05800000", Buy, 1, Limit, Day, away("SPX261016P05800000", 0.82)));
      } else if (time == at(wednesday, 21, 0)) {
        // Thursday's overnight session, after the rollover: a DAY limit lasts until 09:25.
        send(time, batch->received, order("overnight", "SPXW260918C06000000", Sell, 1, Limit, Day, away("SPXW260918C06000000", 1.15)));
      }
    }
    ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
    const auto view = desk.trading_view();
    ASSERT_TRUE(view);
    const auto& snapshot = *view->snapshot;
    for (const auto* id : {"held", "gtc", "day", "short", "curb-limit", "overnight"}) {
      ASSERT_TRUE(replies.contains(id)) << id;
      EXPECT_TRUE(replies.at(id).decision.ok()) << id << ": " << replies.at(id).decision.message;
    }
    EXPECT_EQ(replies.at("curb-market").decision.code, trading::Reason::LIMIT_ONLY);
    // The day rolled over once, at Thursday's first (overnight) batch, on Wednesday's
    // marks, and the end-of-day drawdown floor ratcheted from Wednesday's close.
    ASSERT_EQ(snapshot.evaluation.days.size(), 1U);
    const auto& closed = snapshot.evaluation.days.front();
    EXPECT_EQ(closed.day, wednesday);
    EXPECT_EQ(snapshot.evaluation.day, thursday);
    EXPECT_EQ(closed.peak, std::max(plan->initial_cash, closed.close_equity));
    EXPECT_EQ(closed.floor, closed.peak - plan->rules.max_drawdown);
    EXPECT_EQ(snapshot.evaluation.floor, closed.floor);
    const auto* gtc = find(snapshot, "gtc");
    const auto* day = find(snapshot, "day");
    const auto* curb = find(snapshot, "curb-limit");
    const auto* overnight = find(snapshot, "overnight");
    ASSERT_TRUE(gtc && day && curb && overnight);
    EXPECT_EQ(gtc->status, trading::OrderStatus::Working);
    EXPECT_EQ(day->status, trading::OrderStatus::Cancelled);
    EXPECT_EQ(day->day_end, at(wednesday, 16, 15));
    EXPECT_EQ(curb->status, trading::OrderStatus::Cancelled);
    EXPECT_EQ(curb->day_end, at(wednesday, 17, 0));
    EXPECT_EQ(overnight->status, trading::OrderStatus::Working);
    EXPECT_EQ(overnight->day_end, at(thursday, 9, 25));
    // The call bought on Wednesday is still held on Thursday.
    EXPECT_TRUE(std::any_of(snapshot.positions.begin(), snapshot.positions.end(), [](const auto& p) {
      return p.position.contract.osi_symbol() == md::parse_osi("SPXW260918C06000000")->osi_symbol() && p.position.quantity == 1;
    }));
    // Some short calls were assigned at rollover, delivering short shares held
    // into SPY's ex-date, which owe its dividend.
    ASSERT_EQ(snapshot.stocks.size(), 1U);
    const auto shares = snapshot.stocks.front().position.shares;
    EXPECT_LT(shares, 0);
    EXPECT_EQ(shares % 100, 0);
    ASSERT_EQ(snapshot.dividends.size(), 1U);
    EXPECT_EQ(snapshot.dividends.front().ex_date, thursday);
    EXPECT_EQ(snapshot.dividends.front().shares, shares);
    EXPECT_EQ(snapshot.dividends.front().amount, Money::parse("1.75") * shares);
    desk.stop();
  }
  // The first batch starts Wednesday; Thursday's overnight session rolls it over.
  std::vector<std::pair<json, md::Timestamp>> rollovers;
  for (const auto& record : trading::FileJournal::read(journal.string()).records) {
    if (record.type != "day_rollover") continue;
    const auto payload = json::parse(record.payload);
    for (const auto& event : payload.at("events"))
      if (event.at("type") == "day_rollover") rollovers.emplace_back(event.at("payload").at("day"), record.time);
  }
  ASSERT_EQ(rollovers.size(), 2U);
  EXPECT_EQ(rollovers[0].first, json({{"year", 2026}, {"month", 9}, {"day", 16}}));
  EXPECT_EQ(rollovers[1].first, json({{"year", 2026}, {"month", 9}, {"day", 17}}));
  EXPECT_EQ(rollovers[1].second, windows[2].first);
  const auto verified = server::verify_run(journal);
  EXPECT_TRUE(verified.matched) << verified.message;
}

TEST(MultiDayReplay, StartsAndStepsToADateAndTimeInTheRunsSessions) {
  test::RecordingFile file;
  const auto scenarios = file.directory / "scenarios";
  std::filesystem::create_directory(scenarios);
  { std::ofstream out(scenarios / "weekend.json"); out << kWeekend; }
  server::Engine::Options base;
  base.analytics_interval = std::chrono::milliseconds(1);
  base.paper_journal = file.directory / "paper.jsonl";
  server::ReplayHost host({file.directory, base, true, scenarios});
  const md::Date friday{2026, 11, 27}, sunday{2026, 11, 29}, monday{2026, 11, 30};
  const auto at = [](md::Date date, int hour, int minute) { return md::format_timestamp(md::new_york_to_utc(date, hour, minute)); };
  const auto listing = json::parse(call(host, "GET", "/api/replay").body);
  const auto demo = std::find_if(listing["demos"].begin(), listing["demos"].end(), [](const json& d) { return d["id"] == "weekend"; });
  ASSERT_NE(demo, listing["demos"].end());
  const auto sessions = json::array({
      {{"session", "regular"}, {"date", "2026-11-27"}, {"open", at(friday, 9, 30)}, {"end", at(friday, 13, 15)}},
      {{"session", "overnight"}, {"date", "2026-11-30"}, {"open", at(sunday, 20, 15)}, {"end", at(monday, 9, 25)}},
      {{"session", "regular"}, {"date", "2026-11-30"}, {"open", at(monday, 9, 30)}, {"end", at(monday, 16, 15)}}});
  EXPECT_EQ((*demo)["sessions"], sessions);
  EXPECT_EQ((*demo)["end"], at(monday, 16, 15));
  EXPECT_EQ((*demo)["session"], "regular");
  // A bare time in none of the sessions, and a date and time after the run, are refused.
  EXPECT_EQ(call(host, "POST", "/api/replay", {{"scenario", "weekend"}, {"start_at", "17:30"}}).status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", {{"scenario", "weekend"}, {"start_at", "2026-12-01T09:30"}}).status, 400);
  // A bare start_at is its first occurrence in the run: Friday afternoon.
  const auto started = call(host, "POST", "/api/replay",
      {{"scenario", "weekend"}, {"seed", "scenario"}, {"plan", "eod-50k"}, {"paused", true}, {"start_at", "13:00"}});
  ASSERT_EQ(started.status, 201) << started.body;
  EXPECT_EQ(json::parse(started.body)["replay"]["sessions"], sessions);
  EXPECT_EQ(json::parse(started.body)["replay"]["end"], at(monday, 16, 15));
  const auto replay = [&] { return json::parse(call(host, "GET", "/api/replay").body)["replay"]; };
  ASSERT_TRUE(test::recording_eventually([&] { return !replay()["fast_forwarding"].get<bool>(); }));
  EXPECT_EQ(replay()["time"], at(friday, 13, 0));
  // In a run of several sessions a bare until is the time's next occurrence: Sunday evening.
  const auto evening = call(host, "PUT", "/api/replay", {{"until", "21:00"}});
  ASSERT_EQ(evening.status, 200) << evening.body;
  EXPECT_EQ(json::parse(evening.body)["settled_through"], at(sunday, 21, 0));
  EXPECT_EQ(call(host, "PUT", "/api/replay", {{"until", "17:30"}}).status, 400);
  EXPECT_EQ(call(host, "PUT", "/api/replay", {{"until", "2026-12-01T10:00"}}).status, 400);
  // A date and time is New York's, seconds optional.
  const auto open = call(host, "PUT", "/api/replay", {{"until", "2026-11-30T09:30"}});
  ASSERT_EQ(open.status, 200) << open.body;
  EXPECT_EQ(json::parse(open.body)["settled_through"], at(monday, 9, 30));
  // The account carried over the weekend: Friday closed and Monday is under way.
  const auto account = json::parse(call(host, "GET", "/api/replay/account").body);
  EXPECT_EQ(account["evaluation"]["day"], "2026-11-30");
  ASSERT_EQ(account["evaluation"]["days"].size(), 1U);
  EXPECT_EQ(account["evaluation"]["days"][0]["day"], "2026-11-27");
  host.stop();
  // The finished run keeps its sessions in history.
  const auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history[0]["sessions"], sessions);
}

}  // namespace
