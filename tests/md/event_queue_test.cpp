#include "openport/md/event_queue.hpp"

#include <gtest/gtest.h>

namespace {
using namespace openport::md;

std::vector<Event> drain(EventQueue& queue) {
  std::vector<Event> events;
  queue.drain(events, std::chrono::milliseconds(0));
  return events;
}

TEST(EventQueue, CoalescesEachKindInPlaceAndDefinitionsAreBarriers) {
  EventQueue queue;
  queue.publish(ContractDefinition{0, {}});
  queue.publish(OptionQuote{0, 1});
  queue.publish(VendorGreeks{0, 1});
  queue.publish(OpenInterest{0, 1, 10});
  queue.publish(OpenInterest{0, 2, 11});
  queue.publish(OptionQuote{0, 2});
  queue.publish(VendorGreeks{0, 2});
  queue.publish(UnderlyingQuote{"SPY", 1});
  queue.publish(UnderlyingQuote{"SPY", 2});
  queue.publish(ContractDefinition{0, {}});
  queue.publish(OptionQuote{0, 3});
  queue.publish(VendorGreeks{0, 3});
  queue.publish(OpenInterest{0, 3, 12});
  queue.publish(OpenInterest{0, 4, 13});
  queue.publish(OptionQuote{0, 4});
  EXPECT_EQ(queue.status().depth, 9u);
  EXPECT_EQ(queue.status().coalesced, 6u);
  auto events = drain(queue);
  ASSERT_EQ(events.size(), 9u);
  EXPECT_TRUE(std::holds_alternative<ContractDefinition>(events[0]));
  EXPECT_EQ(std::get<OptionQuote>(events[1]).ts, 2);
  EXPECT_EQ(std::get<VendorGreeks>(events[2]).ts, 2);
  EXPECT_EQ(std::get<OpenInterest>(events[3]).contracts, 11);
  EXPECT_EQ(std::get<UnderlyingQuote>(events[4]).ts, 2);
  EXPECT_TRUE(std::holds_alternative<ContractDefinition>(events[5]));
  EXPECT_EQ(std::get<OptionQuote>(events[6]).ts, 4);
  EXPECT_EQ(std::get<VendorGreeks>(events[7]).ts, 3);
  EXPECT_EQ(std::get<OpenInterest>(events[8]).contracts, 13);
  EXPECT_EQ(queue.status().depth, 0u);
  queue.publish(OptionQuote{0, 5});
  EXPECT_EQ(drain(queue).size(), 1u);
  EXPECT_EQ(queue.status().coalesced, 6u);
}

TEST(EventQueue, DropsOnlyTradesAndRetainsFirstStateUpdatesBeyondCapacity) {
  EventQueue queue(3);
  queue.publish(OptionTrade{0, 1});
  queue.publish(OptionQuote{0, 2});
  queue.publish(OptionTrade{0, 3});
  queue.publish(OptionTrade{0, 4});          // discard incoming trade
  queue.publish(UnderlyingQuote{"SPY", 5});  // evict oldest queued trade
  queue.publish(ContractDefinition{0, {}});  // evict remaining trade
  queue.publish(OptionQuote{1, 6});          // no trades left; retain the first quote anyway
  queue.publish(OpenInterest{0, 7, 10});
  queue.publish(OpenInterest{0, 8, 11});
  queue.publish(VendorGreeks{1, 8});
  queue.publish(UnderlyingQuote{"SPX", 8});
  queue.publish(ProviderStatus{9, FeedState::Error, "offline", "SPY"});
  EXPECT_EQ(queue.status().depth, 8u);
  EXPECT_EQ(queue.status().dropped, 3u);
  EXPECT_TRUE(queue.status().overloaded);
  auto events = drain(queue);
  ASSERT_EQ(events.size(), 8u);
  EXPECT_EQ(std::get<OptionQuote>(events[0]).ts, 2);
  EXPECT_EQ(std::get<UnderlyingQuote>(events[1]).ts, 5);
  EXPECT_TRUE(std::holds_alternative<ContractDefinition>(events[2]));
  EXPECT_EQ(std::get<OptionQuote>(events[3]).ts, 6);
  EXPECT_EQ(std::get<OpenInterest>(events[4]).contracts, 11);
  EXPECT_EQ(std::get<VendorGreeks>(events[5]).ts, 8);
  EXPECT_EQ(std::get<UnderlyingQuote>(events[6]).symbol, "SPX");
  EXPECT_TRUE(std::holds_alternative<ProviderStatus>(events[7]));
  EXPECT_FALSE(queue.status().overloaded);
  EXPECT_EQ(queue.status().dropped, 3u);
}

TEST(EventQueue, LongQuoteAndTradeBacklogsStayWithinCapacity) {
  EventQueue queue(8);
  for (int i = 0; i < 10000; ++i) {
    queue.publish(OptionQuote{0, i});
    queue.publish(OptionTrade{0, i});
  }
  EXPECT_EQ(queue.status().depth, 8u);
  EXPECT_EQ(queue.status().coalesced, 9999u);
  EXPECT_EQ(queue.status().dropped, 9993u);
  auto events = drain(queue);
  EXPECT_EQ(std::get<OptionQuote>(events.front()).ts, 9999);
}
}  // namespace

namespace {
TEST(EventQueue, LatestStateAloneCanExceedCapacityWithoutBeingDropped) {
  EventQueue queue(1);
  queue.publish(ContractDefinition{0, {}});
  EXPECT_FALSE(queue.status().overloaded);
  queue.publish(OptionQuote{0, 1});
  queue.publish(VendorGreeks{0, 1});
  queue.publish(UnderlyingQuote{"SPY", 1});
  queue.publish(OpenInterest{0, 1, 10});
  EXPECT_EQ(queue.status().depth, 5u);
  EXPECT_EQ(queue.status().dropped, 0u);
  EXPECT_TRUE(queue.status().overloaded);
  EXPECT_EQ(drain(queue).size(), 5u);
  EXPECT_FALSE(queue.status().overloaded);
  queue.publish(OptionTrade{0, 1});
  queue.publish(OptionTrade{0, 2});
  EXPECT_EQ(queue.status().depth, 1u);
  EXPECT_EQ(queue.status().dropped, 1u);
  EXPECT_TRUE(queue.status().overloaded);  // dropped trade, even without excess depth
  drain(queue);
  EXPECT_FALSE(queue.status().overloaded);
}

TEST(EventQueue, SparseUpdatesAcrossManyDrainsNeverReuseStaleInstrumentOrSpotSlots) {
  EventQueue queue;
  constexpr InstrumentId last_id = 52000;
  // Retain a large instrument index while most subsequent batches touch just one id.
  for (const auto id : {InstrumentId{0}, last_id}) {
    queue.publish(OptionQuote{id, 1});
    queue.publish(VendorGreeks{id, 1});
    queue.publish(OpenInterest{id, 1, 10});
  }
  queue.publish(UnderlyingQuote{"SPX", 1});
  queue.publish(UnderlyingQuote{"SPY", 1});
  ASSERT_EQ(drain(queue).size(), 8u);
  for (int round = 1; round <= 1000; ++round) {
    SCOPED_TRACE(round);
    const auto id = round % 2 == 0 ? InstrumentId{0} : last_id;
    const std::string symbol = round % 2 == 0 ? "SPX" : "SPY";
    // Empty drains also invalidate a generation, without creating any new slots.
    EXPECT_TRUE(drain(queue).empty());
    queue.publish(OptionQuote{id, round});
    queue.publish(VendorGreeks{id, round});
    queue.publish(OpenInterest{id, round, 10});
    queue.publish(UnderlyingQuote{symbol, round});
    queue.publish(OptionQuote{id, round + 1});
    queue.publish(VendorGreeks{id, round + 1});
    queue.publish(OpenInterest{id, round + 1, 11});
    queue.publish(UnderlyingQuote{symbol, round + 1});
    const auto events = drain(queue);
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(std::get<OptionQuote>(events[0]).ts, round + 1);
    EXPECT_EQ(std::get<VendorGreeks>(events[1]).ts, round + 1);
    EXPECT_EQ(std::get<OpenInterest>(events[2]).contracts, 11);
    EXPECT_EQ(std::get<UnderlyingQuote>(events[3]).symbol, symbol);
    EXPECT_EQ(std::get<UnderlyingQuote>(events[3]).ts, round + 1);
  }
  EXPECT_EQ(queue.status().coalesced, 4000u);
  EXPECT_EQ(queue.status().dropped, 0u);
}
}  // namespace
