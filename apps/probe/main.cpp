// openport-probe: connects to a provider, waits for one full snapshot of each
// requested chain, and prints what arrived. The quickest way to check that a
// provider (and your API key) works.
//
//   openport-probe cboe SPX SPY
//   openport-probe cboe SPX --expiries 3 --window 0.05
//   openport-probe cboe SPX --analyze     # also run OpenPort's analytics on the snapshot

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/analytics/chain_book.hpp"
#include "openport/md/event_queue.hpp"
#include "openport/md/recording.hpp"
#include "openport/providers/factory.hpp"
#include "openport/providers/options.hpp"
#include "state.hpp"

namespace {

using namespace openport;

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};

struct Chain {
  double price = 0.0;
  std::map<probe::ExpiryKey, std::map<double, std::pair<md::InstrumentId, md::InstrumentId>>>
      expiries;
};

int usage() {
  std::fprintf(
      stderr,
      "usage: openport-probe <provider> <underlying>... [--expiries N] [--window F] "
      "[--seconds S] [--quotes N] [--analyze] [--record FILE] [--option KEY=VALUE]...\n"
      "replay: --option file=PATH [--option speed=1|10|60|max] [--option loop=on|off]\n"
      "streaming: definitions and N quotes per underlying (default 100), else timeout failure\n"
      "polling: one complete snapshot per underlying\n"
      "databento: --expiries and --window must be 0 (whole-chain upstream "
      "subscription)\nproviders:");
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

/// Runs OpenPort's analytics over the snapshot and compares its IVs with the vendor's.
void print_analytics(const analytics::ChainBook& book) {
  for (const auto& [symbol, underlying] : book.underlyings()) {
    const md::Timestamp as_of = underlying.data_time > 0 ? underlying.data_time : underlying.spot_ts;
    const analytics::UnderlyingMetrics m = analytics::analyze(underlying, book, as_of);
    std::printf("\n%s analytics: %d options priced in %.1f ms (as of %s)\n", symbol.c_str(),
                m.options_priced, m.compute_ms, md::format_timestamp(as_of).c_str());
    std::printf("  %-10s %-2s %7s %10s %9s %7s %9s %12s %8s\n", "expiry", "", "days", "forward",
                "rate", "atm iv", "vs vendor", "gex $M/1%", "strikes");
    std::vector<double> all_diffs;
    int shown = 0;
    for (const auto& slice : m.slices) {
      std::vector<double> diffs;
      for (const auto& row : slice.strikes) {
        // Compare the out-of-the-money side, the one the smile is built from.
        const auto& side = row.strike >= slice.forward.forward ? row.call : row.put;
        if (std::isfinite(side.iv) && std::isfinite(side.vendor_iv)) {
          diffs.push_back(std::abs(side.iv - side.vendor_iv) * 100.0);
        }
      }
      std::sort(diffs.begin(), diffs.end());
      all_diffs.insert(all_diffs.end(), diffs.begin(), diffs.end());
      const double rate = -std::log(slice.forward.discount) / slice.years * 100.0;
      if (shown++ < 12) {
        // '*' marks a rate borrowed from the longer expiries rather than fitted to this one.
        std::printf("  %-10s %-2s %7.2f %10.2f %6.2f%%%c %6.2f%% %8.3fvp %12.1f %8zu\n",
                    md::format_date(slice.expiry).c_str(),
                    probe::settlement_label(md::conventions_for_root(slice.root).settlement),
                    slice.years * 365.0, slice.forward.forward, rate,
                    slice.forward.fitted_discount ? ' ' : '*', slice.atm_iv * 100.0,
                    diffs.empty() ? std::nan("") : diffs[diffs.size() / 2], slice.gex / 1e6,
                    slice.strikes.size());
      }
    }
    std::sort(all_diffs.begin(), all_diffs.end());
    if (!all_diffs.empty()) {
      std::printf(
          "  our IV vs vendor IV over %zu out-of-the-money options: median %.3f, p90 %.3f vol "
          "pts\n",
          all_diffs.size(), all_diffs[all_diffs.size() / 2], all_diffs[all_diffs.size() * 9 / 10]);
    }
    std::printf("  exposure: GEX %.1f $M per 1%%, VEX %.1f $M per vol pt, gamma flip %.2f, "
                "call wall %.0f, put wall %.0f\n",
                m.exposure.gex / 1e6, m.exposure.vex / 1e6, m.exposure.gamma_flip,
                m.exposure.call_wall, m.exposure.put_wall);
  }
}

}  // namespace

int run(int argc, char** argv) {
  if (argc < 3) return usage();
  md::ProviderConfig config{argv[1], env_key_for(argv[1]), {}};
  md::Subscription subscription;
  std::filesystem::path record_file;
  int seconds = 60;
  bool analyze = false;
  int minimum_quotes = 100;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--analyze") {
      analyze = true;
    } else if (arg == "--expiries" && i + 1 < argc) {
      subscription.max_expiries = providers::parse_integer(argv[++i], arg, 0);
    } else if (arg == "--window" && i + 1 < argc) {
      subscription.strike_window = providers::parse_fraction(argv[++i], arg);
    } else if (arg == "--quotes" && i + 1 < argc) {
      minimum_quotes = providers::parse_integer(argv[++i], arg, 1);
    } else if (arg == "--seconds" && i + 1 < argc) {
      seconds = providers::parse_integer(argv[++i], arg, 1);
    } else if (arg == "--record" && i + 1 < argc) {
      record_file = argv[++i];
      if (record_file.empty()) throw std::invalid_argument("--record requires a nonempty path");
    } else if (arg == "--option" && i + 1 < argc) {
      const std::string value = argv[++i];
      const auto eq = value.find('=');
      if (eq == std::string::npos) throw std::invalid_argument("--option takes KEY=VALUE");
      config.options[value.substr(0, eq)] = value.substr(eq + 1);
    } else if (arg.starts_with("-")) {
      throw std::invalid_argument("unknown option or missing value: " + arg);
    } else {
      auto symbol = arg;
      for (char& c : symbol) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      subscription.underlyings.push_back(std::move(symbol));
    }
  }
  if (subscription.underlyings.empty()) return usage();

  providers::validate_subscription(config.name, subscription);
  auto provider = providers::make_provider(config);

  const md::Capabilities caps = provider->capabilities();
  std::printf("provider  %.*s (%s%s, trades %s, open interest %s, vendor greeks %s)\n",
              static_cast<int>(provider->name().size()), provider->name().data(),
              caps.realtime ? "real-time" : "delayed ",
              caps.realtime ? "" : (std::to_string(caps.delay.count() / 60) + " min").c_str(),
              caps.trades ? "yes" : "no", caps.open_interest ? "yes" : "no",
              caps.vendor_greeks ? "yes" : "no");

  md::EventQueue queue;
  std::unique_ptr<md::RecordingSink> recorder;
  if (!record_file.empty())
    recorder = std::make_unique<md::RecordingSink>(record_file,
        md::RecordingHeader{std::string(provider->name()), caps, subscription, md::now()},
        queue, md::RecordingSink::Options{});
  const auto started = std::chrono::steady_clock::now();
  // Stop while the queue is still alive, including exceptions during startup/output.
  struct StopProvider {
    md::Provider& provider;
    ~StopProvider() { provider.stop(); }
  } stop_provider{*provider};
  provider->start(subscription, recorder ? static_cast<md::EventSink&>(*recorder) : queue);

  std::unordered_map<md::InstrumentId, md::OptionContract> contracts;
  std::unordered_map<md::InstrumentId, md::OptionQuote> quotes;
  std::unordered_map<md::InstrumentId, md::VendorGreeks> greeks;
  std::map<std::string, md::UnderlyingQuote> underlyings;
  std::map<std::string, std::size_t> counts;
  probe::Readiness readiness(subscription, caps.poll_interval.count() == 0,
                             static_cast<std::size_t>(minimum_quotes));
  bool failed = false;
  bool ended = false;

  analytics::ChainBook book;
  std::vector<md::Event> batch;
  const auto deadline = started + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline && !readiness.all_ready() && !failed && !ended) {
    batch.clear();
    queue.drain(batch, std::chrono::milliseconds(200));
    for (md::Event& event : batch) {
      readiness.apply(event);
      book.apply(event);
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
                     [&](md::UnderlyingClose&) { ++counts["official closes"]; },
                     [&](md::ProviderStatus& e) {
                       std::printf("status    %.*s: %s\n",
                                   static_cast<int>(md::to_string(e.state).size()),
                                   md::to_string(e.state).data(), e.message.c_str());
                       if (e.state == md::FeedState::Error) failed = true;
                       if (e.state == md::FeedState::Stopped && e.underlying.empty()) ended = true;
                     },
                 },
                 event);
    }
  }
  provider->stop();
  if (recorder) {
    recorder->close();
    if (const auto error = recorder->error(); !error.empty()) {
      std::fprintf(stderr, "openport-probe: %s\n", error.c_str());
      failed = true;
    }
  }
  for (const auto& symbol : subscription.underlyings) {
    if (!readiness.ready(symbol))
      std::fprintf(stderr, "openport-probe: %s not ready before timeout, end of recording or provider failure\n",
                   symbol.c_str());
  }

  std::printf("events   ");
  for (const auto& [name, count] : counts) std::printf(" %s=%zu", name.c_str(), count);
  std::printf("\n");

  // Group contracts into chains and print a few strikes around the money.
  std::map<std::string, Chain> chains;
  for (const auto& [id, contract] : contracts) {
    auto& row = chains[contract.underlying].expiries[probe::expiry_key(contract)][contract.strike];
    (contract.type == pricing::OptionType::Call ? row.first : row.second) = id + 1;  // 0 = none
  }
  for (auto& [symbol, chain] : chains) {
    const auto underlying = underlyings.find(symbol);
    chain.price = underlying != underlyings.end() ? underlying->second.last : 0.0;
    std::printf(
        "\n%s  last %.2f  as of %s  (%zu expiries, %s .. %s)\n", symbol.c_str(), chain.price,
        underlying != underlyings.end() ? md::format_timestamp(underlying->second.ts).c_str() : "?",
        chain.expiries.size(), md::format_timestamp(chain.expiries.begin()->first.first).c_str(),
        md::format_timestamp(chain.expiries.rbegin()->first.first).c_str());

    for (const auto& [expiry, strikes] : chain.expiries) {
      std::printf("  expiry %s %s settlement\n", md::format_timestamp(expiry.first).c_str(),
                  probe::settlement_label(expiry.second));
      std::printf("  %21s  %-8s %10s  %21s  %-8s\n", "call bid / ask", "vendor iv", "strike",
                  "put bid / ask", "vendor iv");
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
  }
  if (analyze) print_analytics(book);
  return failed || !readiness.all_ready() ? 1 : 0;
}

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "openport-probe: %s\n", error.what());
    return 2;
  } catch (...) {
    std::fprintf(stderr, "openport-probe: unknown startup failure\n");
    return 2;
  }
}
