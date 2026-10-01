#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig roomy(AccountRules rules = {}) {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
const Order& order(const TradingSession& s, OrderId id) { return s.snapshot()->recent_orders.at(static_cast<std::size_t>(id - 1)); }
OrderChange price(std::string_view limit) { return {{}, m(limit), {}}; }
OrderChange size(Quantity quantity) { return {quantity, {}, {}}; }
OrderChange level(std::string_view trigger) { return {{}, {}, m(trigger)}; }
/// A second contract quoted at the same market time as `f`.
ScriptedMarket beside(const ScriptedMarket& f, std::string_view osi) {
  ScriptedMarket g = f;
  g.contract = *md::parse_osi(osi);
  return g;
}
void seed(TradingSession& s, const ScriptedMarket& g, std::string_view bid, std::string_view ask, double spot = 5000) {
  s.define(g.contract, g.time);
  auto valuation = g.valuation();
  valuation.spot = spot;
  s.on_quotes({g.quote(bid, ask)}, {valuation}, g.time);
}

TEST(TradingOrders, ARestingLimitIsRepricedInPlaceAndTradesOnceMarketable) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("bid", 3, "3.90"), f.time).decision.ok());
  const auto version = s.snapshot()->account_version;
  auto result = s.modify(1, price("4.00"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  EXPECT_EQ(result.order_id, 1u);
  EXPECT_EQ(result.account_version, version + 1);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 1).request.limit_price, m("4.00"));
  EXPECT_EQ(s.snapshot()->recent_orders.size(), 1u) << "the same order, not a replacement";
  ASSERT_TRUE(s.modify(1, price("4.20"), f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 1).filled_quantity, 3);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.20"));
}

TEST(TradingOrders, AQuantityChangeKeepsFillsAndMustExceedThem) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4.00", "4.20", 2);
  ASSERT_TRUE(s.submit(f.limit("part", 5, "4.20"), f.time).decision.ok());
  ASSERT_EQ(order(s, 1).status, OrderStatus::PartiallyFilled);
  ASSERT_EQ(order(s, 1).filled_quantity, 2);
  const auto refused = s.modify(1, size(2), f.time).decision;
  EXPECT_EQ(refused.code, Reason::INVALID_ORDER);
  EXPECT_EQ(refused.actual, 2.0);
  EXPECT_EQ(refused.limit, 2.0);
  EXPECT_EQ(order(s, 1).request.quantity, 5) << "a refused change leaves the order as it was";
  ASSERT_TRUE(s.modify(1, size(3), f.time).decision.ok());
  EXPECT_EQ(order(s, 1).remaining(), 1);
  EXPECT_EQ(order(s, 1).filled_quantity, 2);
  f.next();
  s.on_quotes({f.quote("4.00", "4.20", 10)}, {f.valuation()}, f.time);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 1).filled_quantity, 3);
}

TEST(TradingOrders, NewTermsPassTheChecksANewOrderWould) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buying_power = true;
  auto config = roomy(rules);
  config.initial_cash = m("1000");
  TradingSession s(config, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("two", 2, "4.00"), f.time).decision.ok());  // 801.30 of 1,000
  EXPECT_EQ(s.modify(1, size(3), f.time).decision.code, Reason::BUYING_POWER);
  EXPECT_EQ(order(s, 1).request.quantity, 2);
  EXPECT_EQ(s.modify(1, price("4.03"), f.time).decision.code, Reason::INVALID_TICK);
  EXPECT_EQ(order(s, 1).request.limit_price, m("4.00"));
  EXPECT_EQ(s.modify(1, level("5000"), f.time).decision.code, Reason::INVALID_ORDER);  // no trigger
  EXPECT_EQ(s.modify(1, {}, f.time).decision.code, Reason::INVALID_ORDER);
  EXPECT_EQ(s.modify(9, price("4.00"), f.time).decision.code, Reason::UNKNOWN_ORDER);
  // The account's own feed gate refuses a change as it would an order.
  EXPECT_EQ(s.modify(1, price("3.90"), f.time, {Reason::FEED_STALLED, "stalled", {}, {}, "SPX"}).decision.code,
            Reason::FEED_STALLED);
  ASSERT_TRUE(s.cancel(1, f.time).decision.ok());
  EXPECT_EQ(s.modify(1, price("3.90"), f.time).decision.code, Reason::ORDER_TERMINAL);
  // Only resting orders change: an IOC never rests.
  ASSERT_TRUE(s.submit(f.market("ioc", 1), f.time).decision.ok());
  EXPECT_EQ(s.modify(2, size(2), f.time).decision.code, Reason::ORDER_TERMINAL);
}

TEST(TradingOrders, AnArmedOrderMovesItsLevelAndActivatesWhenTheNewOneIsReached) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto request = f.market("breakout", 2);
  request.trigger = Trigger{TriggerSource::Underlying, TriggerDirection::AtOrAbove, m("5010")};
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  ASSERT_EQ(order(s, 1).status, OrderStatus::Armed);
  EXPECT_EQ(s.modify(1, price("4.20"), f.time).decision.code, Reason::INVALID_ORDER);  // a market order
  ASSERT_TRUE(s.modify(1, size(1), f.time).decision.ok());
  ASSERT_TRUE(s.modify(1, level("5020"), f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 1).request.trigger->level, m("5020"));
  ASSERT_TRUE(s.modify(1, level("4990"), f.time).decision.ok());  // spot 5,000 is at or above it
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 1).filled_quantity, 1);
  EXPECT_EQ(order(s, 1).triggered_at, f.time);
}

TEST(TradingOrders, BracketExitsChangeTheirLevelOrPriceButNotTheirSize) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto entry = f.limit("entry", 2, "4.20");
  entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.50")}, {}},
                          ExitSpec{{}, m("5.00")}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  ASSERT_EQ(order(s, 2).role, OrderRole::StopLoss);
  ASSERT_EQ(order(s, 3).role, OrderRole::TakeProfit);
  ASSERT_TRUE(s.modify(2, level("3.80"), f.time).decision.ok());
  EXPECT_EQ(order(s, 2).request.trigger->level, m("3.80"));
  EXPECT_EQ(s.modify(2, size(1), f.time).decision.code, Reason::INVALID_ORDER);
  ASSERT_TRUE(s.modify(3, price("4.90"), f.time).decision.ok());
  EXPECT_EQ(order(s, 3).request.limit_price, m("4.90"));
  EXPECT_EQ(s.modify(3, price("4.95"), f.time).decision.code, Reason::INVALID_TICK);
  EXPECT_EQ(order(s, 3).request.limit_price, m("4.90"));
  // The moved stop triggers at its new level.
  f.next();
  s.on_quotes({f.quote("3.80", "4.00")}, {f.valuation()}, f.time);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 3).status, OrderStatus::Cancelled);
}

TEST(TradingOrders, ARetryOfAChangedOrderStillGetsTheOrderAsItNowStands) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-retry-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string expected;
  {
    TradingSession s(roomy(), f.time, FileJournal::create(path));
    f.seed(s);
    const auto request = f.limit("rest", 2, "3.90");
    ASSERT_TRUE(s.submit(request, f.time).decision.ok());
    const auto untouched = s.snapshot_json();
    ASSERT_TRUE(s.modify(1, price("4.00"), f.time).decision.ok());
    ASSERT_TRUE(s.modify(1, size(3), f.time).decision.ok());
    // The retry of the original submission records nothing and answers with the changed order.
    const auto version = s.snapshot()->account_version;
    const auto retry = s.submit(request, f.time);
    EXPECT_TRUE(retry.decision.ok());
    EXPECT_TRUE(retry.replayed);
    EXPECT_EQ(retry.order_id, 1u);
    EXPECT_EQ(retry.account_version, version);
    EXPECT_EQ(s.snapshot()->recent_orders.size(), 1u);
    EXPECT_EQ(s.preview(request, f.time).decision.code, Reason::NONE);
    // Other terms, the changed ones included, still conflict.
    EXPECT_EQ(s.submit(f.limit("rest", 3, "4.00"), f.time).decision.code, Reason::DUPLICATE_CLIENT_ID);
    // An order never changed keeps its journal bytes.
    EXPECT_EQ(untouched.find("submitted"), std::string::npos);
    expected = s.snapshot_json();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  EXPECT_TRUE(recovered.submit(f.limit("rest", 2, "3.90"), f.time).replayed);
  std::filesystem::remove_all(directory);
}

TEST(TradingOrders, CancelAllTakesEveryOpenOrderOrOneUnderlyings) {
  ScriptedMarket f;
  const auto xsp = beside(f, "XSP261022C00500000");
  TradingSession s(roomy(), f.time);
  f.seed(s);
  seed(s, xsp, "4.00", "4.20", 500);
  ASSERT_TRUE(s.submit(f.limit("spx", 1, "3.90"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(xsp.limit("xsp", 1, "3.90"), f.time).decision.ok());
  ASSERT_TRUE(s.cancel_all(std::string("SPX"), f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Cancelled);
  EXPECT_EQ(order(s, 1).reason.code, Reason::USER_CANCEL);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
  ASSERT_TRUE(s.cancel_all(std::nullopt, f.time).decision.ok());
  EXPECT_EQ(order(s, 2).status, OrderStatus::Cancelled);
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
}

TEST(TradingOrders, ClosingPositionsClosesASpreadTogetherAndLeavesOtherUnderlyings) {
  ScriptedMarket f;
  const auto upper = beside(f, "SPXW261022C05010000");
  const auto xsp = beside(f, "XSP261022C00500000");
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  seed(s, upper, "3.00", "3.20");
  seed(s, xsp, "4.00", "4.20", 500);
  // A bear call spread: short the 5000 call, long the 5010 call; and XSP alone.
  ASSERT_TRUE(s.submit(upper.market("long", 1), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("short", 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(xsp.market("xsp", 1), f.time).decision.ok());
  ASSERT_TRUE(s.submit(upper.limit("resting", 1, "3.00"), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->positions.size(), 3u);
  const auto before = s.snapshot()->recent_orders.size();
  const auto result = s.close_positions(std::string("SPX"), f.time);
  ASSERT_TRUE(result.decision.ok());
  const auto& orders = s.snapshot()->recent_orders;
  EXPECT_EQ(order(s, 4).status, OrderStatus::Cancelled);  // the resting SPX order
  // The short and the long that covers it close as one order: buy the short back, sell the long.
  ASSERT_EQ(orders.size(), before + 1);
  const auto& close = orders[before];
  ASSERT_EQ(close.request.legs.size(), 2u);
  EXPECT_EQ(close.request.legs[0], (Leg{f.symbol(), Side::Buy, 1}));
  EXPECT_EQ(close.request.legs[1], (Leg{upper.symbol(), Side::Sell, 1}));
  EXPECT_EQ(close.request.type, OrderType::Market);
  EXPECT_EQ(close.request.tif, TimeInForce::Day);
  EXPECT_EQ(close.request.quantity, 1);
  EXPECT_EQ(close.status, OrderStatus::Filled);
  EXPECT_TRUE(close.reduce_only);
  EXPECT_FALSE(close.system) << "a flatten is the trader's own order";
  EXPECT_EQ(close.request.client_order_id.rfind("openport-close-", 0), 0u);
  ASSERT_EQ(s.snapshot()->positions.size(), 1u);
  EXPECT_EQ(s.snapshot()->positions[0].position.contract.underlying, "XSP");
  EXPECT_TRUE(result.residuals.empty());
}

TEST(TradingOrders, AClosingOrderTheIntegrationRefusesIsRecordedAndTheRestStillClose) {
  ScriptedMarket f;
  const auto xsp = beside(f, "XSP261022C00500000");
  TradingSession s(roomy(), f.time);
  f.seed(s);
  seed(s, xsp, "4.00", "4.20", 500);
  ASSERT_TRUE(s.submit(f.market("spx", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(xsp.market("xsp", 1), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("spx exit", 2, "4.60", Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(xsp.limit("xsp exit", 1, "4.60", Side::Sell), f.time).decision.ok());
  const auto before = s.snapshot()->recent_orders.size();
  const Decision stalled{Reason::FEED_STALLED, "XSP feed stalled", {}, {}, "XSP"};
  const auto result = s.close_positions(std::nullopt, f.time, {{"XSP", stalled}});
  ASSERT_TRUE(result.decision.ok());
  // The refused underlying keeps its exit; the other's is cancelled to close.
  EXPECT_EQ(order(s, 3).reason.code, Reason::USER_CANCEL);
  EXPECT_EQ(order(s, 4).status, OrderStatus::Working);
  const auto& orders = s.snapshot()->recent_orders;
  ASSERT_EQ(orders.size(), before + 2);
  for (auto i = before; i < orders.size(); ++i) {
    const auto& o = orders[i];
    if (o.request.symbol == xsp.symbol()) {
      EXPECT_EQ(o.status, OrderStatus::Rejected);
      EXPECT_EQ(o.reason.code, Reason::FEED_STALLED);
    } else {
      EXPECT_EQ(o.status, OrderStatus::Filled);
      EXPECT_EQ(o.filled_quantity, 2);
    }
  }
  ASSERT_EQ(s.snapshot()->positions.size(), 1u);
  EXPECT_EQ(s.snapshot()->positions[0].position.contract.underlying, "XSP");
  // The position left open says why.
  ASSERT_EQ(result.residuals.size(), 1u);
  EXPECT_EQ(result.residuals[0].symbol, xsp.symbol());
  EXPECT_EQ(result.residuals[0].quantity, 1);
  EXPECT_EQ(result.residuals[0].working, 0);
  EXPECT_EQ(result.residuals[0].reason.code, Reason::FEED_STALLED);
  // Nothing to close is not an error.
  EXPECT_TRUE(s.close_positions(std::string("SPX"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.size(), before + 2);
  // When nothing in scope can close, the flatten is refused and changes nothing.
  const auto refused = s.close_positions(std::nullopt, f.time, {{"XSP", stalled}});
  EXPECT_EQ(refused.decision.code, Reason::FEED_STALLED);
  EXPECT_EQ(refused.decision.scope, "XSP");
  EXPECT_EQ(order(s, 4).status, OrderStatus::Working);
  EXPECT_EQ(s.snapshot()->recent_orders.size(), before + 2);
}

Quantity held(const TradingSession& s, const std::string& symbol) {
  for (const auto& p : s.snapshot()->positions) if (p.position.contract.osi_symbol() == symbol) return p.position.quantity;
  return 0;
}

TEST(TradingOrders, OneFlattenGetsASpreadFlatOnAThinQuoteWithoutLeavingAShortNaked) {
  // Five bull put spreads; the short put's ask then shows 2 contracts at a time.
  // Custom rules without buying power or defined risk would allow a naked short.
  for (const auto latency : {std::int64_t{0}, std::int64_t{1000}}) {
    ScriptedMarket f;
    f.contract = *md::parse_osi("SPXW261022P05000000");
    auto lower = beside(f, "SPXW261022P04990000");
    AccountRules rules;
    rules.fill_latency_ms = latency;
    TradingSession s(roomy(rules), f.time);
    seed(s, f, "4.00", "4.20");
    seed(s, lower, "3.00", "3.20");
    const auto next = [&](Quantity short_ask_size) {
      f.next(); lower.next();
      s.on_quotes({f.quote("4.00", "4.20", short_ask_size), lower.quote("3.00", "3.20")}, {f.valuation(), lower.valuation()}, f.time);
      // Every short is covered at every step.
      EXPECT_LE(-held(s, f.symbol()), held(s, lower.symbol()));
    };
    ASSERT_TRUE(s.submit(f.market("short", 5, Side::Sell), f.time).decision.ok());
    ASSERT_TRUE(s.submit(lower.market("long", 5), f.time).decision.ok());
    if (latency) next(10);
    ASSERT_EQ(held(s, f.symbol()), -5);
    ASSERT_EQ(held(s, lower.symbol()), 5);
    next(2);
    const auto result = s.close_positions(std::nullopt, f.time);
    ASSERT_TRUE(result.decision.ok());
    const auto close = s.snapshot()->recent_orders.back().id;
    ASSERT_EQ(order(s, close).request.legs.size(), 2u) << "one order for the spread";
    EXPECT_EQ(order(s, close).request.quantity, 5);
    // Without latency two spreads close at once; the rest is worked on later quotes.
    const Quantity first = latency ? 0 : 2;
    EXPECT_EQ(held(s, f.symbol()), -5 + first);
    EXPECT_EQ(held(s, lower.symbol()), 5 - first);
    ASSERT_EQ(result.residuals.size(), 2u);
    for (const auto& residual : result.residuals) {
      EXPECT_EQ(residual.working, 5 - first) << residual.symbol;
      EXPECT_TRUE(residual.reason.ok()) << residual.reason.message;
    }
    next(2);
    next(2);
    if (latency) next(2);
    EXPECT_EQ(order(s, close).status, OrderStatus::Filled);
    EXPECT_TRUE(s.snapshot()->positions.empty()) << "one flatten gets flat";
  }
}

TEST(TradingOrders, AFlattenSplitsAPositionLargerThanTheOrderLimit) {
  ScriptedMarket f;
  auto config = roomy();
  config.limits.max_order_contracts = 4;
  TradingSession s(config, f.time);
  f.seed(s, "4.00", "4.20", 20);
  for (const auto* id : {"a", "b", "c"}) ASSERT_TRUE(s.submit(f.market(id, id == std::string("c") ? 2 : 4), f.time).decision.ok());
  ASSERT_EQ(held(s, f.symbol()), 10);
  const auto before = s.snapshot()->recent_orders.size();
  ASSERT_TRUE(s.close_positions(std::nullopt, f.time).decision.ok());
  const auto& orders = s.snapshot()->recent_orders;
  ASSERT_EQ(orders.size(), before + 3);
  EXPECT_EQ(orders[before].request.quantity, 4);
  EXPECT_EQ(orders[before + 1].request.quantity, 4);
  EXPECT_EQ(orders[before + 2].request.quantity, 2);
  for (auto i = before; i < orders.size(); ++i) EXPECT_EQ(orders[i].status, OrderStatus::Filled) << orders[i].reason.message;
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingOrders, AFlattenKeepsTheExitsOfWhatItHasNotClosedYet) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto entry = f.market("entry", 5);
  entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.00")}, {}}, ExitSpec{{}, m("6.00")}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  ASSERT_EQ(held(s, f.symbol()), 5);
  const auto stop = order(s, 1).stop_loss, target = order(s, 1).take_profit;
  ASSERT_NE(stop, 0u);
  ASSERT_NE(target, 0u);
  f.next();
  s.on_quotes({f.quote("4.00", "4.20", 2)}, {f.valuation()}, f.time);
  const auto result = s.close_positions(std::nullopt, f.time);
  ASSERT_TRUE(result.decision.ok());
  // Two sold at the bid; the exits stay, sized to the three still held, while the close works.
  EXPECT_EQ(held(s, f.symbol()), 3);
  EXPECT_TRUE(order(s, stop).open());
  EXPECT_TRUE(order(s, target).open());
  EXPECT_EQ(order(s, stop).remaining(), 3);
  EXPECT_EQ(order(s, target).remaining(), 3);
  const auto close = s.snapshot()->recent_orders.back();
  EXPECT_TRUE(close.reduce_only);
  EXPECT_EQ(close.status, OrderStatus::PartiallyFilled);
  ASSERT_EQ(result.residuals.size(), 1u);
  EXPECT_EQ(result.residuals[0].quantity, 3);
  EXPECT_EQ(result.residuals[0].working, 3);
  EXPECT_TRUE(result.residuals[0].reason.ok());
  // The kill switch leaves a close alone.
  ASSERT_TRUE(s.trip_kill("pause", f.time).decision.ok());
  EXPECT_TRUE(order(s, close.id).open());
  f.next();
  s.on_quotes({f.quote("4.00", "4.20", 10)}, {f.valuation()}, f.time);
  EXPECT_EQ(order(s, close.id).status, OrderStatus::Filled);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(order(s, stop).reason.code, Reason::POSITION_CLOSED);
  EXPECT_EQ(order(s, target).reason.code, Reason::POSITION_CLOSED);
}

TEST(TradingOrders, AFlattenWaitsForQuotesAndGetsPastAPositionAwaitingSettlement) {
  // 15:50 on the expiry day of a PM series that stops at 16:00; a later series too.
  ScriptedMarket today;
  today.contract = *md::parse_osi("SPXW260922C05000000");
  today.time = md::new_york_to_utc({2026, 9, 22}, 15, 50);
  auto later = beside(today, "SPXW261022C05000000");
  TradingSession s(roomy(), today.time);
  seed(s, today, "4.00", "4.20");
  seed(s, later, "6.00", "6.20");
  ASSERT_TRUE(s.submit(today.market("today", 1), today.time).decision.ok());
  ASSERT_TRUE(s.submit(later.market("later", 2), today.time).decision.ok());
  // After 16:00 the expiring series waits for its settlement, and the later one's
  // quote has gone stale: other orders are refused until both are marked again.
  later.time = md::new_york_to_utc({2026, 9, 22}, 16, 5);
  EXPECT_EQ(s.submit(later.limit("refused", 1, "6.20"), later.time).decision.code, Reason::STALE_QUOTE);
  const auto result = s.close_positions(std::nullopt, later.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  const auto close = s.snapshot()->recent_orders.back();
  EXPECT_EQ(close.request.symbol, later.symbol());
  EXPECT_EQ(close.status, OrderStatus::Working) << "a stale quote only makes the close wait";
  ASSERT_EQ(result.residuals.size(), 2u);
  for (const auto& residual : result.residuals) {
    if (residual.symbol == today.symbol()) {
      EXPECT_EQ(residual.working, 0);
      EXPECT_EQ(residual.reason.code, Reason::AWAITING_SETTLEMENT);
    } else {
      EXPECT_EQ(residual.working, 2);
      EXPECT_TRUE(residual.reason.ok());
    }
  }
  later.next();
  s.on_quotes({later.quote("6.00", "6.20")}, {later.valuation()}, later.time);
  EXPECT_EQ(order(s, close.id).status, OrderStatus::Filled);
  EXPECT_EQ(held(s, later.symbol()), 0);
  EXPECT_EQ(held(s, today.symbol()), 1);
}

TEST(TradingOrders, AFlattenBuysBackAShortQuotedOnlyOnTheAskAndGivesAWorthlessLongAway) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPXW261022P04000000");
  auto wing = beside(f, "SPXW261022C06000000");
  TradingSession s(roomy(), f.time);
  seed(s, f, "0.05", "0.10");
  seed(s, wing, "0.05", "0.10");
  ASSERT_TRUE(s.submit(f.market("short", 3, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(wing.market("long", 2), f.time).decision.ok());
  // Nobody bids for either now: 0.00/0.05.
  f.next(); wing.next();
  const auto ask_only = [](const ScriptedMarket& g) {
    return QuoteObservation{g.symbol(), g.observation, g.time, std::nullopt, m("0.05"), 0, 10};
  };
  s.on_quotes({ask_only(f), ask_only(wing)}, {f.valuation(), wing.valuation()}, f.time);
  const auto result = s.close_positions(std::nullopt, f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_TRUE(result.residuals.empty());
  const auto& fills = s.snapshot()->recent_fills;
  ASSERT_GE(fills.size(), 2u);
  const auto buy_back = fills[fills.size() - 2], gift = fills.back();
  EXPECT_EQ(buy_back.symbol, f.symbol());
  EXPECT_EQ(buy_back.price, m("0.05"));
  EXPECT_EQ(buy_back.quantity, 3);
  EXPECT_EQ(gift.symbol, wing.symbol());
  EXPECT_EQ(gift.price, Money{});
  EXPECT_EQ(gift.quantity, 2);
  // An ordinary order still needs a two-sided quote.
  EXPECT_EQ(s.submit(f.market("opening", 1), f.time).decision.code, Reason::INVALID_QUOTE);
}

TEST(TradingOrders, AWorkingFlattenCloseRecoversFromTheJournal) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-orders-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "session.jsonl").string();
  ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    f.seed(s, "4.00", "4.20", 1);
    ASSERT_TRUE(s.submit(f.limit("open", 1, "4.20"), f.time).decision.ok());
    f.next();
    s.on_quotes({f.quote("4.00", "4.20", 1)}, {f.valuation()}, f.time);
    ASSERT_TRUE(s.submit(f.limit("more", 1, "4.20"), f.time).decision.ok());
    ASSERT_EQ(held(s, f.symbol()), 2);
    f.next();
    s.on_quotes({f.quote("4.00", "4.20", 1)}, {f.valuation()}, f.time);
    ASSERT_TRUE(s.close_positions(std::nullopt, f.time).decision.ok());
    ASSERT_EQ(held(s, f.symbol()), 1);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path, head));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  const auto close = recovered.snapshot()->open_orders.at(0);
  EXPECT_TRUE(close.reduce_only);
  f.next();
  recovered.on_quotes({f.quote("4.00", "4.20", 1)}, {f.valuation()}, f.time);
  EXPECT_EQ(order(recovered, close.id).status, OrderStatus::Filled);
  EXPECT_TRUE(recovered.snapshot()->positions.empty());
  std::filesystem::remove_all(directory);
}

TEST(TradingOrders, AFlattenNeverTakesAClientIdTheTraderAlreadyUsed) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 1), f.time).decision.ok());
  // The trader's resting sell takes the ID the next flatten would generate first.
  const auto taken = "openport-close-" + std::to_string(s.snapshot()->account_version + 2) + "-1";
  ASSERT_TRUE(s.submit(f.limit(taken, 1, "4.40", Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.close_positions(std::nullopt, f.time).decision.ok());
  EXPECT_EQ(order(s, 2).reason.code, Reason::USER_CANCEL);
  const auto& close = s.snapshot()->recent_orders.back();
  EXPECT_EQ(close.status, OrderStatus::Filled) << close.reason.message;
  EXPECT_NE(close.request.client_order_id, taken);
  EXPECT_EQ(close.request.client_order_id.rfind("openport-close-", 0), 0u);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingOrders, ChangesAndFlattensRecoverFromTheJournal) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-orders-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "session.jsonl").string();
  ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.limit("rest", 2, "3.90"), f.time).decision.ok());
    ASSERT_TRUE(s.modify(1, price("4.20"), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.limit("again", 1, "3.90"), f.time).decision.ok());
    ASSERT_TRUE(s.close_positions(std::nullopt, f.time).decision.ok());
    expected = s.snapshot_json();
    head = journal->head();
  }
  const auto recovered = TradingSession::recover(FileJournal::read(path, head));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  EXPECT_TRUE(recovered.snapshot()->positions.empty());
  std::filesystem::remove_all(directory);
}


TEST(TradingOrders, EachOrderKeepsWhenItEndedAndEveryChangeAskedOfIt) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-orders-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "lifecycle.jsonl").string();
  ScriptedMarket f;
  std::string expected;
  {
    TradingSession s(roomy(), f.time, FileJournal::create(path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.limit("rest", 3, "3.90"), f.time).decision.ok());
    EXPECT_EQ(order(s, 1).ended_at, 0) << "open orders have not ended";
    f.next();
    s.set_actor("trader");
    ASSERT_TRUE(s.modify(1, price("4.00"), f.time).decision.ok());
    s.set_actor("trader");
    EXPECT_EQ(s.modify(1, size(0), f.time).decision.code, Reason::INVALID_ORDER);
    const auto& changes = order(s, 1).changes;
    ASSERT_EQ(changes.size(), 2u) << "a refused change is kept beside the applied one";
    EXPECT_EQ(changes[0].time, f.time);
    EXPECT_EQ(changes[0].actor, "trader");
    EXPECT_EQ(changes[0].limit_price, m("4.00"));
    EXPECT_EQ(changes[0].previous_limit_price, m("3.90"));
    EXPECT_EQ(changes[0].previous_quantity, 3);
    EXPECT_FALSE(changes[0].quantity);
    EXPECT_TRUE(changes[0].decision.ok());
    EXPECT_EQ(changes[1].quantity, 0);
    EXPECT_EQ(changes[1].decision.code, Reason::INVALID_ORDER);
    EXPECT_EQ(order(s, 1).request.quantity, 3) << "and leaves the order's terms as they were";
    f.next();
    ASSERT_TRUE(s.cancel(1, f.time).decision.ok());
    EXPECT_EQ(order(s, 1).ended_at, f.time);
    // A fill ends an order at the fill's time; a rejection at its submission.
    f.next();
    ASSERT_TRUE(s.submit(f.limit("take", 1, "4.20"), f.time).decision.ok());
    EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
    EXPECT_EQ(order(s, 2).ended_at, f.time);
    EXPECT_FALSE(s.submit(f.limit("off-tick", 1, "4.01"), f.time).decision.ok());
    EXPECT_EQ(order(s, 3).ended_at, f.time);
    EXPECT_TRUE(order(s, 2).changes.empty());
    expected = s.snapshot_json();
  }
  const auto recovery = FileJournal::read(path);
  EXPECT_EQ(TradingSession::recover(recovery).snapshot_json(), expected);
  // Orders without either keep their bytes: neither field is written for them.
  const auto first = recovery.records.at(3);  // the first submit, after the definition and the quotes
  EXPECT_EQ(first.type, "submit");
  EXPECT_EQ(first.payload.find("\"changes\""), std::string::npos);
  EXPECT_EQ(first.payload.find("\"ended_at\""), std::string::npos);
  std::filesystem::remove_all(directory);
}

TEST(TradingOrders, AWorkingOrderSaysWhatItWaitsFor) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4.00", "4.20", 2);
  const auto waiting = [&](OrderId id) {
    const auto& all = s.snapshot()->waiting;
    const auto it = all.find(id);
    return it == all.end() ? OrderWait{} : it->second;
  };
  ASSERT_TRUE(s.submit(f.limit("behind", 1, "3.90"), f.time).decision.ok());
  EXPECT_EQ(waiting(1).code, "LIMIT");
  EXPECT_EQ(waiting(1).message, "The ask 4.20 is above the limit 3.90");
  auto stop = f.market("stop");
  stop.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrAbove, m("5.00")};
  ASSERT_TRUE(s.submit(stop, f.time).decision.ok());
  EXPECT_EQ(waiting(2).code, "TRIGGER");
  EXPECT_EQ(waiting(2).message, "Waits for the ask to reach at or above 5.00 (now 4.20)");
  // The first marketable limit takes both displayed contracts; the second waits for a new quote.
  ASSERT_TRUE(s.submit(f.limit("first", 2, "4.20"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("second", 1, "4.20"), f.time).decision.ok());
  EXPECT_EQ(order(s, 3).status, OrderStatus::Filled);
  EXPECT_EQ(waiting(3).code, "") << "finished orders wait for nothing";
  EXPECT_EQ(waiting(4).code, "DISPLAYED_SIZE");
  // A one-sided book supplies no liquidity.
  f.next();
  s.on_quotes({{f.symbol(), f.observation, f.time, std::nullopt, m("4.20"), 0, 5}}, {f.valuation()}, f.time);
  EXPECT_EQ(waiting(4).code, "INVALID_QUOTE");
  // A GTC limit accepted overnight waits for the regular session.
  ScriptedMarket night = f;
  night.time = md::new_york_to_utc({2026, 9, 22}, 21, 0);
  ++night.observation;
  s.on_quotes({night.quote("4.00", "4.20")}, {night.valuation()}, night.time);
  ASSERT_TRUE(s.submit(night.limit("gtc", 1, "4.20", Side::Buy, TimeInForce::Gtc), night.time).decision.ok());
  EXPECT_EQ(waiting(5).code, "REGULAR_SESSION");
  EXPECT_EQ(waiting(5).message, "GTC orders fill in the regular session only");
}

TEST(TradingOrders, AnOrderHeldByFillLatencySaysUntilWhen) {
  ScriptedMarket f;
  AccountRules rules;
  rules.fill_latency_ms = 1000;
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("late", 1, "4.20"), f.time).decision.ok());
  const auto& wait = s.snapshot()->waiting.at(1);
  EXPECT_EQ(wait.code, "FILL_LATENCY");
  EXPECT_EQ(wait.message, "Fill latency holds it for a quote stamped at or after 10:00:01 ET");
  f.next();
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_FALSE(s.snapshot()->waiting.contains(1));
}

}  // namespace
}  // namespace openport::trading
