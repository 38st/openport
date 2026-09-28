#include "openport/server/equity.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace openport::server {
namespace {
using trading::Money;
std::string amount(const std::optional<Money>& value) { return value ? std::to_string(value->micros()) : "null"; }
std::string line(const EquitySample& s) {
  return std::to_string(s.time) + "," + std::to_string(s.attempt) + "," + std::to_string(s.equity.micros()) + "," +
      amount(s.floor) + "," + std::to_string(s.peak.micros()) + "," + amount(s.target) + "," +
      amount(s.tomorrow_floor) + "," + std::to_string(s.fill) + "," + std::to_string(s.stock_fill) + "\n";
}
EquitySample parse(std::string text) {
  std::replace(text.begin(), text.end(), ',', ' ');
  std::istringstream stream(text);
  EquitySample sample;
  std::int64_t equity = 0, peak = 0;
  std::string floor, target, tomorrow, extra;
  if (!(stream >> sample.time >> sample.attempt >> equity >> floor >> peak >> target >> tomorrow >> sample.fill) ||
      sample.time <= 0 || sample.attempt == 0) throw std::runtime_error("invalid equity sample");
  // The original eight-column format predates share-change samples.
  stream >> std::ws;
  if (!stream.eof() && (!(stream >> sample.stock_fill) || stream >> extra)) throw std::runtime_error("invalid equity sample");
  sample.equity = Money::from_micros(equity);
  sample.peak = Money::from_micros(peak);
  const auto money = [](const std::string& value) -> std::optional<Money> {
    if (value == "null") return {};
    std::size_t used = 0;
    const auto number = std::stoll(value, &used);
    if (used != value.size()) throw std::runtime_error("invalid equity amount");
    return Money::from_micros(number);
  };
  sample.floor = money(floor); sample.target = money(target); sample.tomorrow_floor = money(tomorrow);
  return sample;
}
}
std::vector<EquitySample> fill_equity_samples(const trading::TradingSession& session, const trading::TradingSnapshot& before, bool include_pre_fill) {
  const auto after = session.snapshot();
  if (!after->valuation_complete || after->journal_failed || after->evaluation.attempt != before.evaluation.attempt) return {};
  const auto first = before.recent_fills.size();
  if (first >= after->recent_fills.size() && before.stock_fills.size() >= after->stock_fills.size()) return {};
  std::vector<Money> changes;
  Money total;
  for (auto i = first; i < after->recent_fills.size(); ++i) {
    const auto& fill = after->recent_fills[i];
    const auto quote = session.quote(fill.symbol);
    if (!quote || !trading::markable_quote(*quote)) return {};
    const auto mark = quote->bid ? *quote->bid + (*quote->ask - *quote->bid).prorate(1, 2) : quote->ask->prorate(1, 2);
    const auto change = ((mark - fill.price) * 100) * (fill.side == trading::Side::Buy ? fill.quantity : -fill.quantity) - fill.fee;
    changes.push_back(change);
    total = total + change;
  }
  auto equity = after->equity - total;
  auto peak = before.evaluation.peak;
  auto floor = before.evaluation.floor;
  const auto& rules = session.config().rules;
  bool decided = before.evaluation.status != trading::EvaluationStatus::Active;
  const auto ratchet = [&] {
    if (!decided && rules.evaluation() && rules.drawdown_mode == trading::DrawdownMode::Intraday) {
      peak = std::max(peak, equity);
      if (rules.max_drawdown > Money{}) {
        floor = peak - rules.max_drawdown;
        if (rules.lock_balance > Money{}) floor = std::min(floor, rules.lock_balance);
      }
    }
    if ((rules.max_drawdown > Money{} && equity <= floor) ||
        (rules.profit_target > Money{} && equity >= after->evaluation.starting_balance + rules.profit_target)) decided = true;
  };
  ratchet();
  std::vector<EquitySample> result;
  // Optional observation for headless drawdown measurement: a quote can mark a
  // new high and trigger an exit in the same transaction. Preserve that mark
  // before subtracting the committed fills' spread and fees.
  if (include_pre_fill && first < after->recent_fills.size()) {
    EquitySample sample;
    sample.time = after->recent_fills[first].time;
    sample.attempt = after->evaluation.attempt;
    sample.equity = equity;
    sample.peak = peak;
    if (rules.max_drawdown > Money{}) sample.floor = floor;
    result.push_back(sample);
  }
  for (auto i = first; i < after->recent_fills.size();) {
    auto end = i + 1;
    const auto& fill = after->recent_fills[i];
    const auto& order = after->recent_orders.at(static_cast<std::size_t>(fill.order_id - 1));
    if (trading::multi_leg(order.request)) end = std::min(after->recent_fills.size(), i + order.request.legs.size());
    for (auto j = i; j < end; ++j) equity = equity + changes[j - first];
    ratchet();
    for (; i < end; ++i) {
      EquitySample sample;
      sample.time = after->recent_fills[i].time; sample.attempt = after->evaluation.attempt;
      sample.equity = equity; sample.peak = peak; sample.fill = after->recent_fills[i].id;
      if (rules.max_drawdown > Money{}) {
        sample.floor = floor;
        if (rules.drawdown_mode == trading::DrawdownMode::EndOfDay) {
          auto tomorrow = std::max(peak, equity) - rules.max_drawdown;
          if (rules.lock_balance > Money{}) tomorrow = std::min(tomorrow, rules.lock_balance);
          sample.tomorrow_floor = std::max(floor, tomorrow);
        }
      }
      if (rules.profit_target > Money{}) sample.target = after->evaluation.starting_balance + rules.profit_target;
      result.push_back(sample);
    }
  }
  // Deliveries can also remove an option at intrinsic value; use the committed
  // transaction mark for these atomic share changes, including manual closes.
  for (auto i = before.stock_fills.size(); i < after->stock_fills.size(); ++i) {
    const auto& fill = after->stock_fills[i];
    EquitySample sample;
    sample.time = fill.time; sample.attempt = after->evaluation.attempt;
    sample.equity = after->equity; sample.peak = after->evaluation.peak; sample.stock_fill = fill.id;
    if (rules.max_drawdown > Money{}) {
      sample.floor = after->evaluation.floor;
      if (rules.drawdown_mode == trading::DrawdownMode::EndOfDay) {
        auto tomorrow = std::max(sample.peak, sample.equity) - rules.max_drawdown;
        if (rules.lock_balance > Money{}) tomorrow = std::min(tomorrow, rules.lock_balance);
        sample.tomorrow_floor = std::max(*sample.floor, tomorrow);
      }
    }
    if (rules.profit_target > Money{}) sample.target = after->evaluation.starting_balance + rules.profit_target;
    result.push_back(sample);
  }
  return result;
}
EquityStore::EquityStore(std::filesystem::path file) : file_(std::move(file)) {
  try {
    if (!std::filesystem::exists(file_)) return;
    std::ifstream input(file_);
    if (!input) throw std::runtime_error("cannot read equity history");
    std::string text;
    while (std::getline(input, text)) {
      ++lines_;
      // A torn trailing row is ignored; earlier complete rows stay usable.
      try {
        auto sample = parse(text);
        if (samples_.empty() || sample.time >= samples_.back().time) samples_.push_back(std::move(sample));
      } catch (const std::exception&) { error_ = "equity: ignored an invalid row"; }
    }
    compact();
  } catch (const std::exception& e) { error_ = std::string("equity: ") + e.what(); }
}
void EquityStore::compact() {
  if (samples_.empty()) return;
  const auto newest = samples_.back();
  const auto old = newest.time - 90 * md::kNanosPerDay;
  std::size_t earlier = 0;
  for (auto it = samples_.rbegin(); it != samples_.rend(); ++it) if (it->attempt != newest.attempt) ++earlier;
  std::erase_if(samples_, [&](const EquitySample& s) {
    if (s.attempt == newest.attempt) return false;
    if (s.time < old || earlier > 100'000) { --earlier; return true; }
    return false;
  });
  const auto temporary = file_.string() + ".tmp";
  std::ofstream out(temporary, std::ios::trunc);
  if (!out) throw std::runtime_error("cannot compact equity history");
  for (const auto& sample : samples_) out << line(sample);
  out.flush();
  if (!out) throw std::runtime_error("cannot flush equity history");
  out.close();
  std::filesystem::rename(temporary, file_);
  lines_ = samples_.size();
}
void EquityStore::append(const EquitySample& sample) noexcept {
  try {
    if (sample.time <= 0 || sample.attempt == 0) return;
    if (!samples_.empty()) {
      const auto& last = samples_.back();
      if (sample.time < last.time) return;
      if (sample.attempt == last.attempt && sample.fill == 0 && sample.stock_fill == 0 && last.time / md::kNanosPerMinute == sample.time / md::kNanosPerMinute &&
          last.floor == sample.floor && last.target == sample.target) return;
      if (sample.fill != 0 && std::any_of(samples_.rbegin(), samples_.rend(), [&](const EquitySample& s) { return s.fill == sample.fill; })) return;
      if (sample.stock_fill != 0 && std::any_of(samples_.rbegin(), samples_.rend(), [&](const EquitySample& s) { return s.stock_fill == sample.stock_fill; })) return;
    }
    const bool reset = !samples_.empty() && samples_.back().attempt != sample.attempt;
    samples_.push_back(sample);
    std::ofstream out(file_, std::ios::app);
    out << line(sample);
    out.flush();
    if (!out) throw std::runtime_error("cannot append equity history");
    ++lines_;
    if (reset || lines_ % 4096 == 0) compact();
  } catch (const std::exception& e) { error_ = std::string("equity: ") + e.what(); }
}
}  // namespace openport::server
