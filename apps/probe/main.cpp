// openport-probe: connects to a provider, waits for one full snapshot of each
// requested chain, and prints what arrived. The quickest way to check that a
// provider (and your API key) works.
//
//   openport-probe cboe SPX SPY
//   openport-probe cboe SPX --expiries 3 --window 0.05

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "openport/md/event_queue.hpp"
#include "openport/providers/factory.hpp"

namespace {

using namespace openport;

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};

struct Chain {
  double price = 0.0;
  std::map<md::Date, std::map<double, std::pair<md::InstrumentId, md::InstrumentId>>> expiries;
};

int usage() {
  std::fprintf(stderr,
               "usage: openport-probe <provider> <underlying>... [--expiries N] [--window F] "
               "[--seconds S]\nproviders:");
  for (auto name : providers::provider_names()) {
    std::fprintf(stderr, " %.*s", static_cast<int>(name.size()), name.data());
  }
  std::fprintf(stderr, "\n");
  return 2;
}

std::string env_key_for(std::string name) {
  for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const char* value = std::getenv((name + "_API_KEY").c_str());
  return value ? value : "";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  md::ProviderConfig config{argv[1], env_key_for(argv[1]), {}};
  md::Subscription subscription;
  int seconds = 60;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--expiries" && i + 1 < argc) {
      subscription.max_expiries = std::atoi(argv[++i]);
    } else if (arg == "--window" && i + 1 < argc) {
      subscription.strike_window = std::atof(argv[++i]);
    } else if (arg == "--seconds" && i + 1 < argc) {
      seconds = std::atoi(argv[++i]);
    } else if (arg.starts_with("--")) {
      return usage();
    } else {
      subscription.underlyings.push_back(arg);
    }
  }
  if (subscription.underlyings.empty()) return usage();

  std::unique_ptr<md::Provider> provider;
  try {
    provider = providers::make_provider(config);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "openport-probe: %s\n", error.what());
    return 1;
  }
  const md::Capabilities caps = provider->capabilities();
  std::printf("provider  %.*s (%s%s, trades %s, open interest %s, vendor greeks %s)\n",
              static_cast<int>(provider->name().size()), provider->name().data(),
              caps.realtime ? "real-time" : "delayed ",
              caps.realtime ? "" : (std::to_string(caps.delay.count() / 60) + " min").c_str(),
              caps.trades ? "yes" : "no", caps.open_interest ? "yes" : "no",
              caps.vendor_greeks ? "yes" : "no");

  md::EventQueue queue;
  const auto started = std::chrono::steady_clock::now();
  provider->start(subscription, queue);

  std::unordered_map<md::InstrumentId, md::OptionContract> contracts;
  std::unordered_map<md::InstrumentId, md::OptionQuote> quotes;
  std::unordered_map<md::InstrumentId, md::VendorGreeks> greeks;
  std::map<std::string, md::UnderlyingQuote> underlyings;
  std::map<std::string, std::size_t> counts;
  std::size_t snapshots = 0;
  bool failed = false;

  std::vector<md::Event> batch;
  const auto deadline = started + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline &&
         snapshots < subscription.underlyings.size() && !failed) {
    batch.clear();
    queue.drain(batch, std::chrono::milliseconds(200));
    for (md::Event& event : batch) {
      std::visit(Overloaded{
                     [&](md::ContractDefinition& e) {
                       ++counts["contracts"];
                       contracts.emplace(e.id, std::move(e.contract));
                     },
                     [&](md::OptionQuote& e) {
                       ++counts["quotes"];
                       quotes[e.id] = e;
                     },
                     [&](md::OptionTrade&) { ++counts["trades"]; },
                     [&](md::OpenInterest&) { ++counts["open interest"]; },
                     [&](md::VendorGreeks& e) {
                       ++counts["vendor greeks"];
                       greeks[e.id] = e;
                     },
                     [&](md::UnderlyingQuote& e) { underlyings[e.symbol] = e; },
                     [&](md::ProviderStatus& e) {
                       std::printf("status    %.*s: %s\n",
                                   static_cast<int>(md::to_string(e.state).size()),
                                   md::to_string(e.state).data(), e.message.c_str());
                       if (e.state == md::FeedState::Live || e.state == md::FeedState::Delayed) {
                         ++snapshots;
                       }
                       if (e.state == md::FeedState::Error) failed = true;
                     },
                 },
                 event);
    }
  }
  provider->stop();

  std::printf("events   ");
  for (const auto& [name, count] : counts) std::printf(" %s=%zu", name.c_str(), count);
  std::printf("\n");

  // Group contracts into chains and print a few strikes around the money.
  std::map<std::string, Chain> chains;
  for (const auto& [id, contract] : contracts) {
    auto& row = chains[contract.underlying].expiries[contract.expiry][contract.strike];
    (contract.type == pricing::OptionType::Call ? row.first : row.second) = id + 1;  // 0 = none
  }
  for (auto& [symbol, chain] : chains) {
    const auto underlying = underlyings.find(symbol);
    chain.price = underlying != underlyings.end() ? underlying->second.last : 0.0;
    std::printf("\n%s  last %.2f  as of %s  (%zu expiries, %s .. %s)\n", symbol.c_str(),
                chain.price,
                underlying != underlyings.end() ? md::format_timestamp(underlying->second.ts).c_str()
                                                : "?",
                chain.expiries.size(), md::format_date(chain.expiries.begin()->first).c_str(),
                md::format_date(chain.expiries.rbegin()->first).c_str());

    const auto& [expiry, strikes] = *chain.expiries.begin();
    std::printf("  nearest expiry %s\n", md::format_date(expiry).c_str());
    std::printf("  %21s  %-8s %10s  %21s  %-8s\n", "call bid / ask", "cboe iv", "strike",
                "put bid / ask", "cboe iv");
    auto atm = strikes.lower_bound(chain.price);
    for (int back = 0; back < 3 && atm != strikes.begin(); ++back) --atm;
    int shown = 0;
    for (auto it = atm; it != strikes.end() && shown < 7; ++it, ++shown) {
      auto side = [&](md::InstrumentId slot, char* out, std::size_t size, char* iv_out) {
        std::snprintf(out, size, "-");
        std::snprintf(iv_out, 16, "-");
        if (slot == 0) return;
        const md::InstrumentId id = slot - 1;
        if (auto q = quotes.find(id); q != quotes.end()) {
          std::snprintf(out, size, "%9.2f / %-9.2f", q->second.bid, q->second.ask);
        }
        if (auto g = greeks.find(id); g != greeks.end()) {
          std::snprintf(iv_out, 16, "%.2f%%", g->second.iv * 100.0);
        }
      };
      char call[32], put[32], call_iv[16], put_iv[16];
      side(it->second.first, call, sizeof call, call_iv);
      side(it->second.second, put, sizeof put, put_iv);
      std::printf("  %21s  %-8s %10.2f  %21s  %-8s\n", call, call_iv, it->first, put, put_iv);
    }
  }
  return failed || snapshots == 0 ? 1 : 0;
}
