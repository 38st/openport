#include <cstdlib>
#include <filesystem>
#include <tuple>
#include <gtest/gtest.h>

#include "openport/trading/session.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view s) { return Money::parse(s); }
/// Canonical padded OSI.
std::string osi(std::string_view compact) { return md::parse_osi(compact)->osi_symbol(); }
const std::string P4900 = osi("SPXW261022P04900000");
const std::string P4890 = osi("SPXW261022P04890000");
const std::string C5100 = osi("SPXW261022C05100000");
const std::string C5110 = osi("SPXW261022C05110000");
const std::string C5120 = osi("SPXW261022C05120000");
const std::string LATER = osi("SPXW261023P04890000");
const std::string LATER_4900 = osi("SPXW261023P04900000");
const std::string XSP = osi("XSP261022P00490000");

/// A few SPXW strikes around 5000, quoted together.
struct Chain {
  md::Timestamp time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  std::uint64_t observation = 0;
  void define(TradingSession& s, std::initializer_list<std::string> symbols) {
    for (const auto& symbol : symbols) ASSERT_TRUE(s.define(*md::parse_osi(symbol), time).decision.ok());
  }
  /// (symbol, bid, ask, delta); every quote carries `size` on both sides.
  void quote(TradingSession& s, std::vector<std::tuple<std::string, std::string, std::string, double>> books, Quantity size = 10) {
    ++observation;
    time += md::kNanosPerSecond;
    std::vector<QuoteObservation> quotes;
    std::vector<Valuation> valuations;
    for (const auto& [symbol, bid, ask, delta] : books) {
      const auto contract = *md::parse_osi(symbol);
      quotes.push_back({symbol, observation, time, m(bid), m(ask), size, size});
      valuations.push_back({symbol, time, delta, 0.001, 2.0, -0.1, 5000, 5010, 0.99,
                            md::years_between(time, contract.expiry_time()), 0.20, true});
    }
    s.on_quotes(quotes, valuations, time);
  }
};
Leg leg(const std::string& symbol, Side side, Quantity ratio = 1) { return {symbol, side, ratio}; }
OrderRequest combo(std::string client, std::vector<Leg> legs, Quantity units, std::optional<std::string_view> net,
                   TimeInForce tif = TimeInForce::Day) {
  OrderRequest r;
  r.client_order_id = std::move(client);
  r.type = net ? OrderType::Limit : OrderType::Market;
  r.tif = net ? tif : TimeInForce::Ioc;
  r.quantity = units;
  if (net) r.limit_price = m(*net);
  r.legs = std::move(legs);
  return r;
}
SessionConfig config(std::string_view cash = "100000", AccountRules rules = {}) {
  SessionConfig c;
  c.initial_cash = m(cash);
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
MarginLeg margin(const std::string& symbol, Quantity quantity, std::string_view value = "0") {
  return {*md::parse_osi(symbol), quantity, m(value), 5000.0};
}

TEST(TradingMargin, SpreadsNeedTheirWidthAndBoundedGroupsTheirWorstLoss) {
  // Naked: buy-back value plus 100 * max(20% of 5000 - 100 OTM, 10% of 4900) per contract.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500")}), m("90500"));
  EXPECT_EQ(margin_requirement({margin(P4900, -2, "1000")}), m("181000"));
  // A credit put spread holds its width; a debit spread nothing.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(P4890, 1)}), m("1000"));
  EXPECT_EQ(margin_requirement({margin(P4900, 1), margin(P4890, -1, "400")}), m("0"));
  // An iron condor loses on one wing at most.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(P4890, 1), margin(C5100, -1, "300"), margin(C5110, 1)}), m("1000"));
  // A long butterfly cannot lose more than its debit, which is paid.
  EXPECT_EQ(margin_requirement({margin(C5100, 1), margin(C5110, -2, "500"), margin(C5120, 1)}), m("0"));
  // A 1x2 ratio: one short covered, the other naked at half the buy-back value.
  EXPECT_EQ(margin_requirement({margin(P4900, -2, "1000"), margin(P4890, 1)}), m("91500"));
  // A strangle is unbounded above, so both shorts are naked.
  const auto call = naked_requirement(*md::parse_osi(C5100), 5000.0);
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(C5100, -1, "300")}), m("90800") + call);
  // A long that expires with its short or later covers it: diagonals hold the width, calendars nothing.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(LATER, 1)}), m("1000"));
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(LATER_4900, 1)}), m("0"));
  // A long that expires first does not cover a later short.
  EXPECT_EQ(margin_requirement({margin(LATER, -1, "400"), margin(P4900, 1)}), m("400") + naked_requirement(*md::parse_osi(LATER), 5000.0));
  // An iron condor beside a calendar: each expiry on its own still wins.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(P4890, 1), margin(C5100, -1, "300"), margin(C5110, 1),
                                margin(LATER_4900, 1)}), m("1000"));
  EXPECT_EQ(margin_requirement({margin(P4890, 3), margin(C5100, 2)}), Money{});
}

TEST(TradingMultiLeg, CreditSpreadFillsBothLegsTogetherAtTheFarSides) {
  Chain f;
  TradingSession s(config(), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  // Sell 4900 at the bid, buy 4890 at the ask: a net 0.80 credit.
  auto result = s.submit(combo("spread", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 2, "-0.80"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  auto snap = s.snapshot();
  const auto& order = snap->recent_orders.at(0);
  EXPECT_EQ(order.status, OrderStatus::Filled);
  EXPECT_EQ(order.filled_quantity, 2);
  EXPECT_EQ(order.filled_notional, m("-1.60"));
  ASSERT_EQ(snap->recent_fills.size(), 2);
  EXPECT_EQ(snap->recent_fills[0].symbol, P4900);
  EXPECT_EQ(snap->recent_fills[0].side, Side::Sell);
  EXPECT_EQ(snap->recent_fills[0].quantity, 2);
  EXPECT_EQ(snap->recent_fills[0].price, m("5.00"));
  EXPECT_EQ(snap->recent_fills[0].order_id, order.id);
  EXPECT_EQ(snap->recent_fills[1].symbol, P4890);
  EXPECT_EQ(snap->recent_fills[1].price, m("4.20"));
  EXPECT_EQ(snap->account.cash, m("100157.40"));  // + 2 * 100 * 0.80 - 4 * 0.65
  EXPECT_EQ(snap->positions.size(), 2);
  // The spread holds its width, not a naked requirement.
  EXPECT_EQ(snap->buying_power.short_requirement, m("2000"));

  // A 1x2 ratio rests until the book reaches its limit; the legs' sizes bound the units.
  result = s.submit(combo("wider", {leg(P4900, Side::Sell), leg(P4890, Side::Buy, 2)}, 5, "2.00"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  EXPECT_EQ(s.snapshot()->recent_orders.at(1).status, OrderStatus::Working);
  f.quote(s, {{P4900, "8.80", "9.00", -0.40}, {P4890, "4.20", "4.30", -0.30}}, 5);
  snap = s.snapshot();
  const auto& wider = snap->recent_orders.at(1);
  EXPECT_EQ(wider.status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(wider.filled_quantity, 2);  // 5 displayed on 4890 covers two units of two
  EXPECT_EQ(wider.filled_notional, m("-0.40"));  // 2 * (2 * 4.30 - 8.80)
  EXPECT_EQ(snap->recent_fills.at(2).quantity, 2);
  EXPECT_EQ(snap->recent_fills.at(3).quantity, 4);
}

TEST(TradingMultiLeg, OrdersAreCheckedAsAWhole) {
  Chain f;
  auto limits = config();
  limits.limits.max_order_contracts = 20;
  TradingSession s(limits, f.time);
  f.define(s, {P4900, P4890, C5100, XSP});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}, {C5100, "3.00", "3.20", 0.30}, {XSP, "0.50", "0.52", -0.30}});
  const auto spread = std::vector<Leg>{leg(P4900, Side::Sell), leg(P4890, Side::Buy)};
  const auto code = [&](OrderRequest r) { return s.submit(std::move(r), f.time).decision.code; };
  EXPECT_EQ(code(combo("one", {leg(P4900, Side::Sell)}, 1, "-0.80")), Reason::INVALID_ORDER);
  EXPECT_EQ(code(combo("same", {leg(P4900, Side::Sell), leg(P4900, Side::Buy)}, 1, "0.00")), Reason::INVALID_ORDER);
  EXPECT_EQ(code(combo("mixed", {leg(P4900, Side::Sell), leg(XSP, Side::Buy)}, 1, "-4.00")), Reason::INVALID_ORDER);
  EXPECT_EQ(code(combo("ratio", {leg(P4900, Side::Sell, 11), leg(P4890, Side::Buy)}, 1, "-0.80")), Reason::INVALID_ORDER);
  auto triggered = combo("trigger", spread, 1, "-0.80");
  triggered.trigger = Trigger{TriggerSource::Underlying, TriggerDirection::AtOrBelow, m("4900")};
  EXPECT_EQ(code(triggered), Reason::INVALID_ORDER);
  auto named = combo("named", spread, 1, "-0.80");
  named.symbol = P4900;
  EXPECT_EQ(code(named), Reason::INVALID_ORDER);
  EXPECT_EQ(code(combo("tick", spread, 1, "-0.83")), Reason::INVALID_TICK);  // SPXW's smallest tick is 0.05
  EXPECT_EQ(code(combo("size", {leg(P4900, Side::Sell, 3), leg(P4890, Side::Buy, 3)}, 7, "-2.40")), Reason::MAX_ORDER_CONTRACTS);
  EXPECT_EQ(code(combo("band", spread, 1, "-4.00")), Reason::PRICE_BAND);
  EXPECT_EQ(code(combo("zero", spread, 0, "-0.80")), Reason::INVALID_ORDER);
  auto day_market = combo("market-day", spread, 1, std::nullopt);
  day_market.tif = TimeInForce::Day;  // market orders are IOC
  EXPECT_EQ(code(day_market), Reason::INVALID_ORDER);
  // Buy-only plans trade single legs.
  AccountRules buy_only;
  buy_only.buy_only = true;
  TradingSession b(config("100000", buy_only), f.time);
  f.define(b, {P4900, P4890});
  f.quote(b, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_EQ(b.submit(combo("long-legs", {leg(P4900, Side::Buy), leg(P4890, Side::Buy)}, 1, "9.40"), f.time).decision.code, Reason::BUY_ONLY);
}

TEST(TradingMultiLeg, BuyingPowerNetsSpreadsThatANakedShortCouldNotAfford) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(config("10000", rules), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  OrderRequest naked{"naked", P4900, Side::Sell, OrderType::Limit, TimeInForce::Day, 1, m("5.00"), {}, {}, {}};
  EXPECT_EQ(s.submit(naked, f.time).decision.code, Reason::BUYING_POWER);
  // Resting, a credit spread reserves its width less its credit, plus fees.
  auto result = s.submit(combo("rest", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 3, "-1.20"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("2643.90"));  // 3 * (1000 - 120) + 6 * 0.65
  // Ten more units would need more than the account has.
  result = s.submit(combo("too-many", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 9, "-0.80"), f.time);
  EXPECT_EQ(result.decision.code, Reason::BUYING_POWER);
  // Filled, the account keeps the credit and holds the width.
  result = s.submit(combo("fill", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 5, "-0.80"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->account.cash, m("10393.50"));  // + 5 * 80 - 10 * 0.65
  EXPECT_EQ(snap->buying_power.short_requirement, m("5000"));
  EXPECT_EQ(snap->buying_power.available, m("10393.50") - m("5000") - m("2643.90"));
}

OrderRequest single(std::string client, const std::string& symbol, Side side, std::optional<std::string_view> limit = {}) {
  return {std::move(client), symbol, side, limit ? OrderType::Limit : OrderType::Market, limit ? TimeInForce::Day : TimeInForce::Ioc,
          1, limit ? std::optional<Money>(m(*limit)) : std::nullopt, {}, {}, {}};
}

TEST(TradingBuyingPower, LeggingIntoASpreadReservesOnlyItsWidth) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  {
    // Short first: the put is naked until its protection is bought.
    TradingSession s(config("10000", rules), f.time);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    EXPECT_EQ(s.submit(single("short-first", P4900, Side::Sell), f.time).decision.code, Reason::BUYING_POWER);
  }
  TradingSession s(config("10000", rules), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(single("long", P4890, Side::Buy), f.time).decision.ok());
  // Selling against the long reserves the spread's width less the credit, not a naked requirement.
  ASSERT_TRUE(s.submit(single("rest", P4900, Side::Sell, "5.40"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("460.65"));  // 1000 - 540 + 0.65
  ASSERT_TRUE(s.cancel(2, f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("short", P4900, Side::Sell), f.time).decision.ok());
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->positions.size(), 2);
  EXPECT_EQ(snap->account.cash, m("10078.70"));  // - 420 + 500 - 2 * 0.65
  EXPECT_EQ(snap->buying_power.short_requirement, m("1000"));
  EXPECT_EQ(snap->buying_power.available, m("9078.70"));
}

TEST(TradingDefinedRisk, ShortsNeedALongOfTheirTypeExpiringWithThemOrLater) {
  Chain f;
  AccountRules rules;
  rules.defined_risk = true;
  TradingSession s(config("100000", rules), f.time);
  f.define(s, {P4900, P4890, LATER, C5100, C5110});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}, {LATER, "4.50", "4.70", -0.27},
              {C5100, "3.00", "3.20", 0.30}, {C5110, "2.50", "2.70", 0.27}});
  // A lone short is naked, and a call does not cover a put.
  EXPECT_EQ(s.submit(single("naked", P4900, Side::Sell), f.time).decision.code, Reason::DEFINED_RISK);
  ASSERT_TRUE(s.submit(single("call", C5110, Side::Buy), f.time).decision.ok());
  EXPECT_EQ(s.submit(single("still naked", P4900, Side::Sell), f.time).decision.code, Reason::DEFINED_RISK);
  // A long put, of any strike, expiring with the short or later covers it.
  ASSERT_TRUE(s.submit(single("later", LATER, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("covered", P4900, Side::Sell), f.time).decision.ok());
  // Selling the long would uncover the short; closing the short first is always allowed.
  EXPECT_EQ(s.submit(single("uncover", LATER, Side::Sell), f.time).decision.code, Reason::DEFINED_RISK);
  ASSERT_TRUE(s.submit(single("close short", P4900, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("close long", LATER, Side::Sell), f.time).decision.ok());
  // A spread opens as one order; a calendar with the short expiring later does not.
  ASSERT_TRUE(s.submit(combo("spread", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, {}), f.time).decision.ok());
  EXPECT_EQ(s.submit(combo("backwards", {leg(LATER, Side::Sell), leg(P4890, Side::Buy)}, 1, {}), f.time).decision.code,
            Reason::DEFINED_RISK);
  // Without the rule the same lone short goes through.
  TradingSession any(config("100000", {}), f.time);
  f.define(any, {P4900});
  f.quote(any, {{P4900, "5.00", "5.20", -0.30}});
  EXPECT_TRUE(any.submit(single("naked", P4900, Side::Sell), f.time).decision.ok());
}

TEST(TradingDefinedRisk, CountsShortsNoLongCovers) {
  const auto contract = [](std::string_view s) { return *md::parse_osi(s); };
  const auto legs = [&](std::vector<std::pair<std::string_view, Quantity>> held) {
    std::vector<MarginLeg> out;
    for (const auto& [symbol, q] : held) out.push_back({contract(symbol), q, {}, std::nullopt});
    return out;
  };
  EXPECT_EQ(naked_shorts(legs({{"SPXW261022P04900000", -2}, {"SPXW261022P04890000", 1}})), 1);
  EXPECT_EQ(naked_shorts(legs({{"SPXW261022P04900000", -1}, {"SPXW261023P04890000", 1}})), 0);  // a later long
  EXPECT_EQ(naked_shorts(legs({{"SPXW261023P04900000", -1}, {"SPXW261022P04890000", 1}})), 1);  // an earlier one
  EXPECT_EQ(naked_shorts(legs({{"SPXW261022C05100000", -1}, {"SPXW261022P04890000", 1}})), 1);  // another type
  EXPECT_EQ(naked_shorts(legs({{"SPY261022P00500000", -1}, {"SPXW261022P04890000", 1}})), 1);   // another underlying
  // The latest shorts take the earliest long that covers them, covering the most.
  EXPECT_EQ(naked_shorts(legs({{"SPXW261022P04900000", -1}, {"SPXW261023P04900000", -1},
                               {"SPXW261022P04890000", 1}, {"SPXW261023P04890000", 1}})), 0);
}

TEST(TradingBuyingPower, UncoveringAShortNeedsBuyingPowerButClosingItNeverDoes) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(config("10000", rules), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(combo("spread", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, "-0.80"), f.time).decision.ok());
  // Selling the long alone would leave a naked put the account cannot carry.
  const auto uncover = s.submit(single("sell-long", P4890, Side::Sell), f.time).decision;
  EXPECT_EQ(uncover.code, Reason::BUYING_POWER);
  EXPECT_NE(uncover.message.find("uncovers a short"), std::string::npos) << uncover.message;
  // Closing both together frees buying power, so it is always allowed.
  const auto close = s.submit(combo("close", {leg(P4900, Side::Buy), leg(P4890, Side::Sell)}, 1, "1.20"), f.time);
  ASSERT_TRUE(close.decision.ok()) << close.decision.message;
  EXPECT_TRUE(s.snapshot()->positions.empty());
  // So is buying the short back first, then selling the long.
  ASSERT_TRUE(s.submit(combo("again", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, "-0.80"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("buy-short", P4900, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("sell-long-after", P4890, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingBuyingPower, ProtectionIsAllowedWhenBuyingPowerIsNegative) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  auto c = config("100000", rules);
  c.limits.max_daily_loss = m("1000000");
  TradingSession s(c, f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(single("naked", P4900, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.available, m("9989.35"));  // 100499.35 - 510 - 90000
  // A sharp fall: the put is deep in the money and its naked requirement grows.
  ++f.observation;
  f.time += md::kNanosPerSecond;
  std::vector<QuoteObservation> quotes{{P4900, f.observation, f.time, m("119"), m("121"), 10, 10},
                                       {P4890, f.observation, f.time, m("114"), m("116"), 10, 10}};
  std::vector<Valuation> valuations;
  for (const auto& symbol : {P4900, P4890})
    valuations.push_back({symbol, f.time, -0.9, 0.001, 2.0, -0.1, 4800, 4810, 0.99,
                          md::years_between(f.time, md::parse_osi(symbol)->expiry_time()), 0.20, true});
  s.on_quotes(quotes, valuations, f.time);
  EXPECT_EQ(s.snapshot()->buying_power.available, m("-7500.65"));  // 100499.35 - 12000 - 96000
  EXPECT_EQ(s.submit(single("more", P4890, Side::Sell, "114"), f.time).decision.code, Reason::BUYING_POWER);
  // Buying protection frees far more than it costs.
  const auto protect = s.submit(single("protect", P4890, Side::Buy), f.time);
  ASSERT_TRUE(protect.decision.ok()) << protect.decision.message;
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->buying_power.available, m("87898.70"));  // 88898.70 - 1000
}

TEST(TradingMultiLeg, CalendarsHoldOnlyTheirDebit) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(config("2000", rules), f.time);
  f.define(s, {P4900, LATER_4900});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {LATER_4900, "6.00", "6.20", -0.31}});
  // Sell the near put, buy the next day's: a 1.20 debit that the later put covers.
  const auto legs = std::vector<Leg>{leg(P4900, Side::Sell), leg(LATER_4900, Side::Buy)};
  ASSERT_TRUE(s.submit(combo("rest", legs, 5, "1.00"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("506.50"));  // 5 * 100 * 1.00 + 10 * 0.65
  const auto result = s.submit(combo("calendar", legs, 1, "1.20"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->recent_orders.back().status, OrderStatus::Filled);
  EXPECT_EQ(snap->buying_power.short_requirement, Money{});
  EXPECT_EQ(snap->account.cash, m("1878.70"));  // - 120 - 2 * 0.65
}

TEST(TradingMultiLeg, AWorkingComboIsOnePendingExposure) {
  Chain f;
  auto tight = config();
  tight.limits.per_underlying = {100'000, 1e9};
  TradingSession s(tight, f.time);
  f.define(s, {P4900, C5100});
  f.quote(s, {{P4900, "5.00", "5.20", -0.50}, {C5100, "5.00", "5.20", 0.50}});
  // Either leg alone reaches 1 * 100 * 0.5 * 5000 = $250,000 of delta.
  OrderRequest call{"call", C5100, Side::Buy, OrderType::Limit, TimeInForce::Day, 1, m("4.60"), {}, {}, {}};
  EXPECT_EQ(s.submit(call, f.time).decision.code, Reason::DELTA_LIMIT);
  // Together, a straddle's legs offset and fill together.
  const auto result = s.submit(combo("straddle", {leg(P4900, Side::Buy), leg(C5100, Side::Buy)}, 1, "9.00"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  const auto risk = s.snapshot()->risk.underlyings.at("SPX");
  EXPECT_DOUBLE_EQ(risk.reachable.delta_low, 0);
  EXPECT_DOUBLE_EQ(risk.reachable.delta_high, 0);
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Working);
}

TEST(TradingMultiLeg, CombosSurviveRecoveryAndCancelAtTheSessionEnd) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-multileg-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "combo.jsonl").string();
  Chain f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(config(), f.time, journal);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    ASSERT_TRUE(s.submit(combo("rest", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, "-0.95"), f.time).decision.ok());
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto s = TradingSession::recover(FileJournal::read(path, head), FileJournal::resume(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  const auto order = s.snapshot()->open_orders.at(0);
  ASSERT_EQ(order.request.legs.size(), 2);
  EXPECT_EQ(order.request.legs[1].symbol, P4890);
  EXPECT_EQ(order.request.limit_price, m("-0.95"));
  EXPECT_EQ(order_symbols(order.request), (std::vector<std::string>{P4900, P4890}));
  // A DAY combo ends with its legs' regular session.
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 20);
  ASSERT_TRUE(s.on_quotes({}, {}, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.at(0).reason.code, Reason::DAY_END);
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace openport::trading
