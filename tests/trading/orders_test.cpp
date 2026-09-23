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

TEST(TradingOrders, ClosingPositionsBuysShortsBackFirstAndLeavesOtherUnderlyings) {
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
  ASSERT_TRUE(s.close_positions(std::string("SPX"), f.time).decision.ok());
  const auto& orders = s.snapshot()->recent_orders;
  EXPECT_EQ(order(s, 4).status, OrderStatus::Cancelled);  // the resting SPX order
  ASSERT_EQ(orders.size(), before + 2);
  const auto& first = orders[before];
  const auto& second = orders[before + 1];
  EXPECT_EQ(first.request.symbol, f.symbol());
  EXPECT_EQ(first.request.side, Side::Buy);
  EXPECT_EQ(first.request.type, OrderType::Market);
  EXPECT_EQ(first.status, OrderStatus::Filled);
  EXPECT_EQ(second.request.symbol, upper.symbol());
  EXPECT_EQ(second.request.side, Side::Sell);
  EXPECT_EQ(second.status, OrderStatus::Filled);
  EXPECT_FALSE(first.system) << "a flatten is the trader's own order";
  EXPECT_NE(first.request.client_order_id, second.request.client_order_id);
  ASSERT_EQ(s.snapshot()->positions.size(), 1u);
  EXPECT_EQ(s.snapshot()->positions[0].position.contract.underlying, "XSP");
}

TEST(TradingOrders, AClosingOrderTheIntegrationRefusesIsRecordedAndTheRestStillClose) {
  ScriptedMarket f;
  const auto xsp = beside(f, "XSP261022C00500000");
  TradingSession s(roomy(), f.time);
  f.seed(s);
  seed(s, xsp, "4.00", "4.20", 500);
  ASSERT_TRUE(s.submit(f.market("spx", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(xsp.market("xsp", 1), f.time).decision.ok());
  const auto before = s.snapshot()->recent_orders.size();
  const auto result = s.close_positions(std::nullopt, f.time, {{"XSP", {Reason::FEED_STALLED, "XSP feed stalled", {}, {}, "XSP"}}});
  ASSERT_TRUE(result.decision.ok());
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
  // Nothing to close is not an error.
  EXPECT_TRUE(s.close_positions(std::string("SPX"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.size(), before + 2);
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

}  // namespace
}  // namespace openport::trading
