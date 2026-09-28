#include "support/recording.hpp"

#include <condition_variable>
#include <set>

#include "openport/providers/demo_feed.hpp"
#include "openport/providers/factory.hpp"
#include "openport/server/engine.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;

class DemoClock final : public providers::ReplayClock {
 public:
  TimePoint now() override { const std::lock_guard lock(mutex_); return now_; }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [&] { return stop.load() || deadline <= limit_; });
    if (stop.load()) return false;
    now_ = std::max(now_, deadline);
    return true;
  }
  void interrupt() override { const std::lock_guard lock(mutex_); wake_.notify_all(); }
  void through(std::chrono::seconds elapsed) {
    const std::lock_guard lock(mutex_);
    limit_ = TimePoint{} + elapsed;
    wake_.notify_all();
  }
 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  TimePoint now_{}, limit_{};
};
template<class Predicate> bool eventually(Predicate predicate) {
  const auto end = std::chrono::steady_clock::now() + 60s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(2ms);
  }
  return true;
}
providers::DemoProvider::Options settings(std::shared_ptr<DemoClock> clock = {}) {
  providers::DemoProvider::Options options;
  options.days = {"trend", "reversal"};
  options.started = md::new_york_to_utc({2026, 9, 19}, 12, 0);
  options.clock = std::move(clock);
  return options;
}

TEST(DemoFeed, FactoryOptionsSymbolsAndDefaultRotation) {
  const auto names = providers::provider_names();
  EXPECT_NE(std::find(names.begin(), names.end(), "demo"), names.end());
  auto provider = providers::make_provider({"demo", "", {}});
  auto* demo = dynamic_cast<providers::DemoProvider*>(provider.get());
  ASSERT_NE(demo, nullptr);
  EXPECT_EQ(demo->name(), "demo");
  EXPECT_TRUE(providers::simulated_provider(demo->name()));
  EXPECT_EQ(demo->capabilities().poll_interval, 15s);
  EXPECT_TRUE(demo->capabilities().open_interest);
  EXPECT_FALSE(demo->capabilities().realtime);
  EXPECT_EQ(demo->symbols(), (std::vector<std::string>{"SPX", "SPY", "QQQ"}));
  std::vector<std::string> expected, actual;
  for (const auto& day : providers::builtin_scenarios()) if (!day.overnight) expected.push_back(day.id);
  for (const auto& day : demo->days()) actual.push_back(day.id);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(actual.size(), 12U);
  for (const auto speed : {1, 2, 5, 10, 30, 60, 120, 300}) {
    EXPECT_NO_THROW((void)providers::make_provider({"demo", "", {{"speed", std::to_string(speed)}}}));
  }
  for (const auto& option : std::vector<std::pair<std::string, std::string>>{
      {"speed", "max"}, {"speed", "0"}, {"speed", "3"}, {"speed", "1.0"}, {"speed", "301"},
      {"days", ""}, {"days", "trend,"}, {"days", ",trend"}, {"days", "missing"},
      {"days", "trend,overnight"}, {"days", "overnight-gap"}, {"loop", "on"}}) {
    EXPECT_THROW((void)providers::make_provider({"demo", "", {option}}), std::invalid_argument);
  }
  provider = providers::make_provider({"demo", "", {{"days", "chop,trend,chop"}}});
  demo = dynamic_cast<providers::DemoProvider*>(provider.get());
  ASSERT_NE(demo, nullptr);
  ASSERT_EQ(demo->days().size(), 3U);
  EXPECT_EQ(demo->days()[0].id, "chop");
  EXPECT_EQ(demo->days()[1].id, "trend");
  EXPECT_EQ(demo->days()[2].id, "chop");
  EXPECT_NO_THROW(providers::validate_subscription("demo", {{"SPX", "QQQ"}}));
  EXPECT_THROW(providers::validate_subscription("demo", {{"IWM"}}), std::invalid_argument);
  EXPECT_THROW(providers::validate_subscription("demo", {{"SPX"}, 1}), std::invalid_argument);
  EXPECT_THROW(providers::validate_subscription("demo", {{"SPX"}, 0, .1}), std::invalid_argument);
  EXPECT_THROW(demo->validate({{}}), std::invalid_argument);
  test::DiscardEvents sink;
  EXPECT_THROW(demo->start({{"DIA"}}, sink), std::invalid_argument);
}

TEST(DemoFeed, DatesUseNewYorkWeekendsHolidaysAndRepeatableSeeds) {
  providers::DemoProvider provider(settings());
  EXPECT_EQ(provider.first_date(), (md::Date{2026, 9, 18}));
  EXPECT_EQ(providers::DemoProvider::next_date({2026, 9, 18}), (md::Date{2026, 9, 21}));
  EXPECT_EQ(providers::DemoProvider::next_date({2026, 7, 2}), (md::Date{2026, 7, 6}));
  EXPECT_EQ(providers::DemoProvider::next_date({2026, 12, 24}), (md::Date{2026, 12, 28}));
  auto options = settings();
  options.started = *md::parse_datetime("2026-09-22T02:00:00Z", md::Zone::Utc);
  providers::DemoProvider monday(options);
  EXPECT_EQ(monday.first_date(), (md::Date{2026, 9, 18}));
  const auto seed = providers::DemoProvider::seed("trend", {2026, 9, 18});
  EXPECT_EQ(seed, 6721558778999182104ULL);
  EXPECT_NE(seed, providers::DemoProvider::seed("trend", {2026, 9, 21}));
  EXPECT_NE(seed, providers::DemoProvider::seed("chop", {2026, 9, 18}));
  for (const auto& scenario : providers::builtin_scenarios()) {
    if (scenario.overnight) continue;
    const auto shortened = providers::DemoProvider::on_date(scenario, {2026, 11, 27});
    for (const auto& event : shortened.events) {
      if (event.type != "gap") {
        EXPECT_LT(providers::scenario_time(event.at, {2026, 11, 27}, false), md::new_york_to_utc({2026, 11, 27}, 13, 0));
      }
    }
  }
  const auto& builtins = providers::builtin_scenarios();
  const auto pin = std::find_if(builtins.begin(), builtins.end(), [](const auto& day) { return day.id == "close-pin"; });
  ASSERT_NE(pin, builtins.end());
  const auto shortened = providers::DemoProvider::on_date(*pin, {2026, 11, 27});
  EXPECT_EQ(shortened.events.front().at, "12:11");
  test::RecordingFile file;
  EXPECT_NO_THROW(providers::write_scenario_recording(file.path, shortened, {2026, 11, 27}, seed));
}

TEST(DemoFeed, RecoveredTimeSelectsTheFollowingTradingDate) {
  providers::DemoProvider provider(settings());
  provider.start_after(md::new_york_to_utc({2026, 12, 24}, 13, 15));
  EXPECT_EQ(provider.first_date(), (md::Date{2026, 12, 28}));
  EXPECT_EQ(provider.time(), md::new_york_to_utc({2026, 12, 28}, 9, 30));
  // After 17:00 ET the saved trading date is already Monday.
  provider.start_after(md::new_york_to_utc({2026, 12, 24}, 18, 0));
  EXPECT_EQ(provider.first_date(), (md::Date{2026, 12, 29}));
  // A saved date before startup also resumes on its following trading date.
  provider.start_after(md::new_york_to_utc({2026, 7, 2}, 16, 15));
  EXPECT_EQ(provider.first_date(), (md::Date{2026, 7, 6}));
  EXPECT_EQ(provider.time(), md::new_york_to_utc({2026, 7, 6}, 9, 30));
  EXPECT_EQ(provider.days()[0].id, "trend");
  EXPECT_EQ(provider.days()[1].id, "reversal");
}

TEST(DemoFeed, RotatesWithoutStoppedStatusPreservesIdsAndDeletesFiles) {
  auto clock = std::make_shared<DemoClock>();
  providers::DemoProvider provider(settings(clock));
  test::EventCollector sink;
  std::mutex mutex;
  std::map<std::string, md::InstrumentId> definitions;
  std::set<md::InstrumentId> used;
  std::map<md::InstrumentId, md::Date> expiries;
  std::size_t retired = 0;
  std::vector<md::Date> dates;
  std::vector<std::string> titles;
  std::vector<double> opens;
  md::Timestamp previous = 0;
  std::size_t overlapping = 0;
  provider.set_driver([&](providers::ReplayBatch batch) {
    const std::lock_guard lock(mutex);
    EXPECT_GE(batch.time, previous);
    previous = batch.time;
    for (const auto& event : batch.events) {
      if (const auto* definition = std::get_if<md::ContractDefinition>(&event)) {
        expiries[definition->id] = definition->contract.expiry;
        const auto [it, inserted] = definitions.try_emplace(definition->contract.osi_symbol(), definition->id);
        if (inserted) { EXPECT_TRUE(used.insert(definition->id).second); }
        else { EXPECT_EQ(it->second, definition->id); ++overlapping; }
      } else if (const auto* quote = std::get_if<md::OptionQuote>(&event)) {
        EXPECT_TRUE(used.contains(quote->id));
        if (quote->bid == 0 && quote->ask == 0 && expiries.at(quote->id) < md::new_york_time(quote->ts).date) ++retired;
      } else if (const auto* status = std::get_if<md::ProviderStatus>(&event)) {
        EXPECT_NE(status->state, md::FeedState::Stopped);
        if (titles.empty() || titles.back() != status->message) titles.push_back(status->message);
      } else if (const auto* spot = std::get_if<md::UnderlyingQuote>(&event)) {
        const auto date = md::new_york_time(spot->ts).date;
        if (dates.empty() || dates.back() != date) { dates.push_back(date); opens.push_back(spot->last); }
      }
    }
    std::promise<void> done; done.set_value(); return done.get_future();
  });
  provider.start({{"SPX"}}, sink);
  EXPECT_THROW(provider.start_after(md::new_york_to_utc({2026, 12, 24}, 13, 15)), std::logic_error);
  EXPECT_THROW(provider.start_after(0), std::logic_error);
  const auto directory = provider.directory();
  EXPECT_EQ(std::filesystem::status(directory).permissions() & std::filesystem::perms::others_all, std::filesystem::perms::none);
  clock->through(24300s);
  ASSERT_TRUE(eventually([&] { const std::lock_guard lock(mutex); return dates.size() >= 2; }));
  EXPECT_FALSE(std::filesystem::exists(directory / "2026-09-18.oprec"));
  clock->through(48600s);
  ASSERT_TRUE(eventually([&] { const std::lock_guard lock(mutex); return dates.size() >= 3; }));
  EXPECT_FALSE(std::filesystem::exists(directory / "2026-09-21.oprec"));
  provider.stop();
  for (const auto& event : sink.snapshot()) {
    if (const auto* status = std::get_if<md::ProviderStatus>(&event)) {
      EXPECT_NE(status->state, md::FeedState::Stopped);
      EXPECT_NE(status->state, md::FeedState::Error) << status->message;
    }
  }
  EXPECT_FALSE(std::filesystem::exists(directory));
  provider.stop();
  EXPECT_THROW(provider.start({{"SPX"}}, sink), std::logic_error);
  EXPECT_THROW(provider.start_after(0), std::logic_error);
  EXPECT_EQ(dates, (std::vector<md::Date>{{2026, 9, 18}, {2026, 9, 21}, {2026, 9, 22}}));
  ASSERT_GE(titles.size(), 3U);
  EXPECT_NE(titles[0].find(provider.days()[0].title), std::string::npos);
  EXPECT_NE(titles[1].find(provider.days()[1].title), std::string::npos);
  EXPECT_EQ(titles[0], titles[2]);
  EXPECT_GT(overlapping, 0U);
  EXPECT_GT(retired, 0U);
  // Starting the same calendar day selects the same generated opening price.
  auto again_clock = std::make_shared<DemoClock>();
  providers::DemoProvider again(settings(again_clock));
  test::EventCollector repeated;
  again.start({{"SPX"}}, repeated);
  ASSERT_TRUE(eventually([&] { return again.time() > 0 && !repeated.snapshot().empty(); }));
  again.stop();
  double opening = 0;
  for (const auto& event : repeated.snapshot()) {
    if (const auto* spot = std::get_if<md::UnderlyingQuote>(&event)) opening = spot->last;
    if (const auto* status = std::get_if<md::ProviderStatus>(&event)) { EXPECT_NE(status->state, md::FeedState::Stopped); }
  }
  EXPECT_EQ(opening, opens.front());
}

server::TradingReply submit(server::Engine& engine, server::TradingCommand command) {
  std::promise<server::TradingReply> done;
  auto future = done.get_future();
  engine.post_trading(std::move(command), [&](auto reply) { done.set_value(std::move(reply)); });
  if (future.wait_for(5min) != std::future_status::ready) throw std::runtime_error("command timed out");
  return future.get();
}

TEST(DemoFeed, NoRecoveredMarketTimeKeepsTheDefaultFirstDate) {
  providers::DemoProvider provider(settings(std::make_shared<DemoClock>()));
  test::RecordingFile journals;
  server::Engine::Options options;
  options.paper_journal = journals.directory / "paper.jsonl";
  options.paper_accounts = journals.directory / "accounts";
  options.journal_io.sync = [](int) { return true; };
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  EXPECT_EQ(provider.first_date(), (md::Date{2026, 9, 18}));
  EXPECT_EQ(engine.status().started, md::new_york_to_utc({2026, 9, 18}, 9, 30));
  EXPECT_TRUE(engine.status().trading.enabled);
  engine.stop();
}

TEST(DemoFeed, RestartAfterLatestMainOrNamedJournalStillFills) {
  for (const bool named_ahead : {false, true}) {
    SCOPED_TRACE(named_ahead ? "named account ahead" : "main account ahead");
    test::RecordingFile journals;
    server::Engine::Options options;
    options.paper_journal = journals.directory / "paper.jsonl";
    options.paper_accounts = journals.directory / "accounts";
    options.record_file = journals.path;
    options.journal_io.sync = [](int) { return true; };
    std::filesystem::create_directory(options.paper_accounts);
    const auto earlier = md::new_york_to_utc({2026, 12, 23}, 16, 15);
    const auto later = md::new_york_to_utc({2026, 12, 24}, 13, 15);
    trading::FileJournal::Options journal_options;
    journal_options.hooks = options.journal_io;
    for (const auto& [path, saved_time] : std::vector<std::pair<std::filesystem::path, md::Timestamp>>{
        {options.paper_journal, named_ahead ? earlier : later},
        {options.paper_accounts / "saved.jsonl", named_ahead ? later : earlier}}) {
      trading::TradingSession session({}, 0, trading::FileJournal::create(path.string(), journal_options));
      ASSERT_TRUE(session.roll_day(saved_time).decision.ok());
    }
    providers::DemoProvider provider(settings(std::make_shared<DemoClock>()));
    EXPECT_EQ(provider.first_date(), (md::Date{2026, 9, 18}));
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    const auto first = md::new_york_to_utc({2026, 12, 28}, 9, 30);
    EXPECT_EQ(provider.first_date(), (md::Date{2026, 12, 28}));
    EXPECT_EQ(engine.status().started, first);
    ASSERT_TRUE(eventually([&] {
      const auto view = engine.trading_view();
      return view && view->snapshot && view->snapshot->time >= first;
    }));
    const auto status = engine.status();
    ASSERT_EQ(status.accounts.size(), 2U);
    for (const auto& account : status.accounts) {
      EXPECT_TRUE(account.trading.enabled) << account.id << ": " << account.trading.reason;
    }
    for (const auto& account : {std::string(server::kMainAccount), std::string("saved")}) {
      const auto view = engine.trading_view(account);
      ASSERT_NE(view, nullptr);
      ASSERT_NE(view->snapshot, nullptr);
      EXPECT_EQ(view->snapshot->time, first);
      EXPECT_EQ(view->snapshot->evaluation.day, (md::Date{2026, 12, 28}));
      server::TradingCommand order;
      order.account = account;
      order.order.client_order_id = "after-restart";
      order.order.symbol = md::parse_osi("SPXW261228C05820000")->osi_symbol();
      order.order.quantity = 1;
      order.order.type = trading::OrderType::Market;
      order.order.tif = trading::TimeInForce::Ioc;
      const auto filled = submit(engine, order);
      ASSERT_TRUE(filled.decision.ok()) << filled.decision.message;
      ASSERT_EQ(filled.view->snapshot->recent_fills.size(), 1U);
      EXPECT_EQ(filled.view->snapshot->recent_fills.front().time, first);
      EXPECT_GT(filled.view->snapshot->recent_fills.front().price, trading::Money{});
    }
    engine.stop();
    md::RecordingReader recording(options.record_file);
    EXPECT_EQ(recording.header().started, first);
  }
}

TEST(DemoFeed, ConsecutiveDaysFillRollAndSettleWithLivePaperAccounts) {
  auto clock = std::make_shared<DemoClock>();
  providers::DemoProvider provider(settings(clock));
  test::RecordingFile journals;
  server::Engine::Options options;
  options.paper_accounts = journals.directory / "accounts";
  options.journal_io.sync = [](int) { return true; };
  options.paper.rules.expiry_cutoff = 0;
  options.write_mode = "token";
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  const auto first = md::new_york_to_utc({2026, 9, 18}, 9, 30);
  ASSERT_TRUE(eventually([&] { const auto view = engine.trading_view(); return view && view->snapshot->time >= first; }));
  EXPECT_EQ(engine.status().trading.write, "token");
  server::TradingCommand order;
  order.order.client_order_id = "expiry";
  order.order.symbol = md::parse_osi("SPXW260918C05820000")->osi_symbol();
  order.order.quantity = 1;
  order.order.type = trading::OrderType::Market;
  order.order.tif = trading::TimeInForce::Ioc;
  auto filled = submit(engine, order);
  ASSERT_TRUE(filled.decision.ok()) << filled.decision.message;
  ASSERT_EQ(filled.view->snapshot->recent_fills.size(), 1U);
  EXPECT_GT(filled.view->snapshot->recent_fills.front().price, trading::Money{});
  server::TradingCommand create;
  create.kind = server::TradingCommand::Kind::CreateAccount;
  create.name = "Auto close";
  create.initial_cash = trading::Money::parse("100000");
  create.rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  const auto account = submit(engine, create);
  ASSERT_TRUE(account.decision.ok()) << account.decision.message;
  order.account = account.account;
  ASSERT_TRUE(submit(engine, order).decision.ok());
  order.account.clear();
  order.order.client_order_id = "carry";
  order.order.symbol = md::parse_osi("SPXW260921C06000000")->osi_symbol();
  ASSERT_TRUE(submit(engine, order).decision.ok());
  clock->through(24285s); // Last snapshot before Friday's 16:15 close.
  const auto late = md::new_york_to_utc({2026, 9, 18}, 16, 14, 45);
  ASSERT_TRUE(eventually([&] { return engine.trading_view()->snapshot->time >= late; }));
  auto view = engine.trading_view();
  EXPECT_EQ(view->snapshot->evaluation.day, (md::Date{2026, 9, 18}));
  EXPECT_EQ(view->snapshot->positions.size(), 1U); // Friday expiry settled, Monday retained.
  EXPECT_GT(view->snapshot->account.cash, trading::Money{});
  ASSERT_EQ(view->snapshot->closures.size(), 1U);
  EXPECT_EQ(view->snapshot->closures.front().kind, trading::ClosureKind::Settlement);
  const auto automatic = engine.trading_view(account.account);
  ASSERT_NE(automatic, nullptr);
  EXPECT_TRUE(automatic->snapshot->positions.empty());
  ASSERT_EQ(automatic->snapshot->recent_fills.size(), 2U);
  EXPECT_EQ(automatic->snapshot->recent_fills.back().time, md::new_york_to_utc({2026, 9, 18}, 15, 55));
  clock->through(24300s);
  const auto monday = md::new_york_to_utc({2026, 9, 21}, 9, 30);
  ASSERT_TRUE(eventually([&] { return engine.trading_view()->snapshot->time >= monday; }));
  view = engine.trading_view();
  EXPECT_EQ(view->snapshot->evaluation.day, (md::Date{2026, 9, 21}));
  EXPECT_EQ(view->snapshot->positions.size(), 1U);
  EXPECT_EQ(engine.status().feed_state, md::FeedState::Live);
  order.order.client_order_id = "close-carry";
  order.order.side = trading::Side::Sell;
  const auto closed = submit(engine, order);
  ASSERT_TRUE(closed.decision.ok()) << closed.decision.message;
  EXPECT_TRUE(closed.view->snapshot->positions.empty());
  order.order.client_order_id = "second-expiry";
  order.order.side = trading::Side::Buy;
  ASSERT_TRUE(submit(engine, order).decision.ok());
  clock->through(48600s);
  const auto second_close = md::new_york_to_utc({2026, 9, 21}, 16, 15);
  ASSERT_TRUE(eventually([&] { const auto metrics = engine.metrics("SPX"); return metrics && metrics->as_of >= second_close; }));
  engine.stop();
  view = engine.trading_view();
  EXPECT_TRUE(view->snapshot->positions.empty());
  ASSERT_EQ(view->snapshot->closures.size(), 2U);
  EXPECT_EQ(view->snapshot->closures.back().time, md::new_york_to_utc({2026, 9, 21}, 16, 0));
  EXPECT_EQ(view->snapshot->closures.back().kind, trading::ClosureKind::Settlement);
}
}  // namespace
