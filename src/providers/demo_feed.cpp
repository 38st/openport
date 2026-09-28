#include "openport/providers/demo_feed.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unistd.h>

#include "openport/providers/factory.hpp"

namespace openport::providers {
namespace {
class DayStatus final : public md::EventSink {
 public:
  explicit DayStatus(md::EventSink& sink) : sink_(sink) {}
  void publish(md::Event event) override {
    const auto* status = std::get_if<md::ProviderStatus>(&event);
    if (status && status->state == md::FeedState::Stopped) return;
    if (status && status->state == md::FeedState::Error) failed = true;
    sink_.publish(std::move(event));
  }
  std::atomic<bool> failed{false};
 private:
  md::EventSink& sink_;
};
}

DemoProvider::DemoProvider(Options options) : options_(std::move(options)) {
  if (options_.speed == 0 || !ReplayProvider::valid_speed(options_.speed))
    throw std::invalid_argument("demo: speed must be 1, 2, 5, 10, 30, 60, 120 or 300");
  const auto& builtins = builtin_scenarios();
  if (options_.days.empty()) {
    for (const auto& day : builtins) if (!day.overnight) days_.push_back(day);
  } else {
    for (const auto& id : options_.days) {
      const auto it = std::find_if(builtins.begin(), builtins.end(), [&](const auto& day) { return day.id == id; });
      if (it == builtins.end()) throw std::invalid_argument("demo: unknown day " + id);
      if (it->overnight) throw std::invalid_argument("demo: day " + id + " is not a regular session");
      days_.push_back(*it);
    }
  }
  for (const auto& day : days_)
    for (const auto& symbol : day.symbols)
      if (std::find(symbols_.begin(), symbols_.end(), symbol) == symbols_.end()) symbols_.push_back(symbol);
  const auto started = options_.started != 0 ? options_.started : md::now();
  first_date_ = md::previous_business_day(md::new_york_time(started).date);
  time_ = scenario_open(days_.front(), first_date_);
}
DemoProvider::~DemoProvider() { stop(); }
md::Capabilities DemoProvider::capabilities() const noexcept {
  md::Capabilities out;
  out.poll_interval = std::chrono::seconds(15);
  out.open_interest = true;
  return out;
}
md::Date DemoProvider::next_date(md::Date date) {
  return md::trading_date(md::new_york_to_utc(date, 18, 0));
}
std::uint64_t DemoProvider::seed(std::string_view id, md::Date date) {
  // FNV-1a has specified bytes and arithmetic; std::hash is not stable across builds.
  std::uint64_t value = 14695981039346656037ULL;
  for (const unsigned char byte : std::string(id) + "|" + md::format_date(date)) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return value;
}
Scenario DemoProvider::on_date(Scenario scenario, md::Date date) {
  const int minutes = md::regular_close_hour(date) * 60 - 570;
  if (minutes != 390) {
    for (auto& event : scenario.events) {
      if (event.type == "gap") continue;
      const auto original = md::new_york_time(scenario_time(event.at, scenario.date, false)).seconds / 60;
      const int at = 570 + (original - 570) * minutes / 390;
      event.at = (at / 60 < 10 ? "0" : "") + std::to_string(at / 60) + ":" +
          (at % 60 < 10 ? "0" : "") + std::to_string(at % 60);
    }
  }
  return scenario;
}
void DemoProvider::validate(const md::Subscription& subscription) const {
  validate_subscription(kDemoProvider, subscription);
  if (subscription.underlyings.empty()) throw std::invalid_argument("demo: no symbols subscribed");
  for (const auto& symbol : subscription.underlyings)
    for (const auto& day : days_)
      if (std::find(day.symbols.begin(), day.symbols.end(), symbol) == day.symbols.end())
        throw std::invalid_argument("demo: symbol " + symbol + " is not covered by day " + day.id);
}
void DemoProvider::set_driver(ReplayProvider::Driver driver) {
  if (started_) throw std::logic_error("demo: install driver before start");
  driver_ = std::move(driver);
}
void DemoProvider::start_after(md::Timestamp recovered_time) {
  if (started_) throw std::logic_error("demo: select first date before start");
  if (recovered_time <= 0) return;
  first_date_ = next_date(md::trading_date(recovered_time));
  time_ = scenario_open(days_.front(), first_date_);
}
void DemoProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  if (started_) throw std::logic_error("DemoProvider::start may be called only once");
  validate(subscription);
  auto pattern = (std::filesystem::temp_directory_path() / "openport-feed-XXXXXX").string();
  const auto* created = ::mkdtemp(pattern.data());
  if (!created) throw std::runtime_error("demo: cannot create private recording directory");
  directory_ = created;
  started_ = true;
  thread_ = std::thread([this, subscription, &sink] { run(subscription, sink); });
}
void DemoProvider::stop() {
  stopping_ = true;
  wake_.notify_all();
  // Only the rotation thread joins the replay. stop() interrupts its wait here.
  if (thread_.joinable()) thread_.join();
  std::error_code error;
  if (!directory_.empty()) std::filesystem::remove_all(directory_, error);
}
void DemoProvider::run(md::Subscription subscription, md::EventSink& sink) {
  try {
    std::map<std::string, md::InstrumentId> ids;
    std::set<md::InstrumentId> prior;
    const auto prepare = [&](std::size_t index, md::Date date) {
      const auto day = on_date(days_[index], date);
      const auto path = directory_ / (md::format_date(date) + ".oprec");
      return std::async(std::launch::async, [day, path, date] {
        write_scenario_recording(path, day, date, seed(day.id, date));
        return path;
      });
    };
    std::size_t index = 0;
    auto date = first_date_;
    auto pending = prepare(index, date);
    while (!stopping_.load()) {
      const auto path = pending.get();
      if (stopping_.load()) break;
      const auto next_index = (index + 1) % days_.size();
      const auto following = next_date(date);
      pending = prepare(next_index, following);
      ReplayProvider::Options playback;
      playback.file = path;
      playback.speed = options_.speed;
      playback.clock = options_.clock;
      auto replay = std::make_shared<ReplayProvider>(std::move(playback));
      std::unordered_map<md::InstrumentId, md::InstrumentId> remap;
      bool first_batch = true;
      const auto title = "demo: " + days_[index].title + " · simulated prices";
      replay->set_driver([&, title](ReplayBatch batch) {
        for (auto& event : batch.events) {
          std::visit([&](auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, md::ContractDefinition>) {
              const auto key = value.contract.osi_symbol();
              if (ids.size() >= std::numeric_limits<md::InstrumentId>::max())
                throw std::runtime_error("demo: too many contracts");
              const auto [it, inserted] = ids.try_emplace(key, static_cast<md::InstrumentId>(ids.size()));
              (void)inserted;
              remap[value.id] = it->second;
              value.id = it->second;
            } else if constexpr (requires { value.id; }) {
              value.id = remap.at(value.id);
            } else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
              value.message = title;
            }
          }, event);
        }
        if (first_batch) {
          // Every definition precedes the first snapshot. Retire quotes omitted
          // by the new day before its snapshot can vouch for the whole chain.
          std::set<md::InstrumentId> current;
          for (const auto& [original, mapped] : remap) { (void)original; current.insert(mapped); }
          std::vector<md::Event> retired;
          for (const auto id : prior)
            if (!current.contains(id)) retired.emplace_back(md::OptionQuote{id, batch.time, 0, 0, 0, 0});
          batch.events.insert(batch.events.begin(), retired.begin(), retired.end());
          prior = std::move(current);
          first_batch = false;
        }
        // SnapshotComplete precedes the generator's status; publish health in the
        // same batch so the next day's first snapshot is immediately healthy.
        if (!batch.events.empty()) {
          if (const auto* complete = std::get_if<md::SnapshotComplete>(&batch.events.back()))
            batch.events.emplace_back(md::ProviderStatus{batch.time, md::FeedState::Live, title, complete->underlying});
        }
        time_ = batch.received;
        if (driver_) return driver_(std::move(batch));
        for (auto& event : batch.events) sink.publish(std::move(event));
        std::promise<void> done;
        done.set_value();
        return done.get_future();
      });
      DayStatus status(sink);
      replay->start(subscription, status);
      {
        std::unique_lock lock(mutex_);
        while (!stopping_.load() && !replay->finished()) wake_.wait_for(lock, std::chrono::milliseconds(10));
      }
      replay->stop();
      std::filesystem::remove(path);
      if (status.failed) break;
      index = next_index;
      date = following;
    }
  } catch (const std::exception& error) {
    sink.publish(md::ProviderStatus{time(), md::FeedState::Error, "demo: " + std::string(error.what()), ""});
  }
}

}  // namespace openport::providers
