#include "openport/providers/scenario.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace openport::providers {
namespace {
using nlohmann::json;
void require(bool condition, const std::string& field, const std::string& message) {
  if (!condition) throw std::invalid_argument(field + ": " + message);
}
void keys(const json& value, std::initializer_list<std::string_view> allowed, const std::string& field) {
  require(value.is_object(), field, "expected an object");
  for (auto it = value.begin(); it != value.end(); ++it)
    require(std::find(allowed.begin(), allowed.end(), it.key()) != allowed.end(), field + "." + it.key(), "unknown field");
}
std::string text(const json& value, const std::string& field, std::size_t limit = 500) {
  require(value.is_string(), field, "expected text");
  auto out = value.get<std::string>();
  require(!out.empty() && out.size() <= limit && out.find_first_of("\n\r") == std::string::npos, field, "expected one nonempty line");
  return out;
}
double number(const json& value, const std::string& field, double low, double high) {
  require(value.is_number(), field, "expected a number");
  const auto n = value.get<double>();
  require(std::isfinite(n) && n >= low && n <= high, field, "outside [" + std::to_string(low) + ", " + std::to_string(high) + "]");
  return n;
}
std::vector<std::filesystem::path> files(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> out;
  if (directory.empty()) return out;
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.is_regular_file() && entry.path().extension() == ".json") out.push_back(entry.path());
  std::sort(out.begin(), out.end());
  return out;
}
}  // namespace

md::Timestamp scenario_time(std::string_view time, md::Date date, bool overnight) {
  require(time.size() == 5 && time[2] == ':' && time[0] >= '0' && time[0] <= '2' &&
          time[1] >= '0' && time[1] <= '9' && time[3] >= '0' && time[3] <= '5' && time[4] >= '0' && time[4] <= '9',
          "time", "expected HH:MM New York time");
  const int hour = (time[0] - '0') * 10 + time[1] - '0';
  const int minute = (time[3] - '0') * 10 + time[4] - '0';
  require(hour < 24, "time", "hour must be below 24");
  if (overnight && hour * 60 + minute >= 20 * 60 + 15) date = md::date_from_days(md::days_since_epoch(date) - 1);
  const auto result = md::new_york_to_utc(date, hour, minute);
  require(result != md::kInvalidTimestamp, "time", "invalid date or DST time");
  return result;
}
md::Timestamp scenario_open(const Scenario& scenario, md::Date date) {
  return scenario_time(scenario.overnight ? "20:15" : "09:30", date, scenario.overnight);
}
md::Timestamp scenario_close(const Scenario& scenario, md::Date date) {
  return scenario.overnight ? scenario_time("09:25", date, true) : md::new_york_to_utc(date, md::regular_close_hour(date), 0);
}

Scenario read_scenario(const std::filesystem::path& file) {
  std::string source;
  try {
    require(std::filesystem::file_size(file) <= 64 * 1024, "$", "file exceeds 64 KiB");
    std::ifstream input(file);
    require(input.good(), "$", "cannot open file");
    source.assign(std::istreambuf_iterator<char>{input}, {});
    require(!input.bad(), "$", "cannot read file");
  } catch (const std::exception& error) {
    throw std::invalid_argument(file.string() + ": " + error.what());
  }
  return parse_scenario(source, file);
}

Scenario parse_scenario(std::string_view source, const std::filesystem::path& file) {
  try {
    require(source.size() <= 64 * 1024, "$", "file exceeds 64 KiB");
    const auto j = json::parse(source);
    keys(j, {"id", "title", "description", "goal", "symbols", "session", "date", "seed", "generator", "drift", "volatility", "iv_shift", "spot_vol", "events"}, "$");
    for (const auto* field : {"id", "title", "description", "symbols", "session", "date", "seed", "generator", "drift", "volatility", "iv_shift", "spot_vol"})
      require(j.contains(field), field, "required field is missing");
    Scenario s;
    s.source = source;
    s.source_file = std::filesystem::absolute(file);
    s.id = text(j.at("id"), "id", 40);
    require(s.id.front() != '-' && s.id.back() != '-' && std::all_of(s.id.begin(), s.id.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    }), "id", "use lowercase letters, digits and interior hyphens");
    s.title = text(j.at("title"), "title", 100);
    s.description = text(j.at("description"), "description");
    if (j.contains("goal")) s.goal = text(j.at("goal"), "goal");
    const auto session = text(j.at("session"), "session");
    require(session == "regular" || session == "overnight", "session", "expected regular or overnight");
    s.overnight = session == "overnight";
    require(j.at("symbols").is_array() && !j.at("symbols").empty(), "symbols", "expected a nonempty array");
    std::set<std::string> seen;
    for (const auto& value : j.at("symbols")) {
      const auto symbol = text(value, "symbols");
      require(symbol == "SPX" || (!s.overnight && (symbol == "SPY" || symbol == "QQQ")), "symbols", "supported: SPX, SPY, QQQ; overnight: SPX only");
      require(seen.insert(symbol).second, "symbols", "duplicate symbol");
      s.symbols.push_back(symbol);
    }
    const auto date = text(j.at("date"), "date");
    const auto parsed = md::parse_datetime(date + "T12:00:00", md::Zone::NewYork);
    require(date.size() == 10 && parsed.has_value(), "date", "expected YYYY-MM-DD");
    s.date = md::new_york_time(*parsed).date;
    require(md::trading_date(*parsed) == s.date, "date", "must be a trading day");
    require(j.at("seed").is_number_unsigned() || (j.at("seed").is_number_integer() && j.at("seed").get<std::int64_t>() >= 0), "seed", "expected uint64");
    s.seed = j.at("seed").get<std::uint64_t>();
    require(j.at("generator").is_number_integer() && j.at("generator") == 1, "generator", "only version 1 is supported");
    require(j.at("drift").is_array() && !j.at("drift").empty() && j.at("drift").size() <= 100, "drift", "expected 1 to 100 waypoints");
    double previous = 0;
    for (const auto& point : j.at("drift")) {
      require(point.is_array() && point.size() == 2, "drift", "expected [fraction, cumulative log return]");
      const double until = number(point[0], "drift.fraction", 0, 1);
      require(until > previous, "drift.fraction", "must be strictly increasing, starting above zero");
      s.drift.emplace_back(until, number(point[1], "drift.move", -0.2, 0.2));
      previous = until;
    }
    require(previous == 1, "drift", "last fraction must be 1");
    s.volatility = number(j.at("volatility"), "volatility", 0, 1);
    s.iv_shift = number(j.at("iv_shift"), "iv_shift", -0.1, 1);
    s.spot_vol = number(j.at("spot_vol"), "spot_vol", -10, 0);
    if (j.contains("events")) {
      require(j.at("events").is_array() && j.at("events").size() <= 32, "events", "expected at most 32 events");
      std::set<std::string> unique;
      for (const auto& e : j.at("events")) {
        require(e.is_object() && e.contains("type"), "events.type", "required field is missing");
        ScenarioEvent event;
        event.type = text(e.at("type"), "events.type");
        if (event.type == "gap") keys(e, {"type", "move"}, "events");
        else if (event.type == "crush") keys(e, {"type", "at", "iv"}, "events");
        else if (event.type == "spike") keys(e, {"type", "at", "move", "iv"}, "events");
        else if (event.type == "pin") keys(e, {"type", "at", "strike"}, "events");
        else require(false, "events.type", "expected gap, crush, spike or pin");
        if (event.type != "gap") {
          require(e.contains("at"), "events.at", "required field is missing");
          event.at = text(e.at("at"), "events.at");
          md::Timestamp at = 0;
          try { at = scenario_time(event.at, s.date, s.overnight); }
          catch (const std::invalid_argument& error) { throw std::invalid_argument("events.at: " + std::string(error.what())); }
          require(at >= scenario_open(s, s.date) && at < scenario_close(s, s.date), "events.at", "must be inside the session before its close");
        }
        require(unique.insert(event.type + event.at).second, "events", "duplicate event type and time");
        if (event.type == "gap" || event.type == "spike") {
          require(e.contains("move"), "events.move", "required field is missing");
          event.move = number(e.at("move"), "events.move", -0.1, 0.1);
        }
        if (event.type == "crush" || event.type == "spike") {
          require(e.contains("iv"), "events.iv", "required field is missing");
          event.iv = number(e.at("iv"), "events.iv", event.type == "crush" ? -0.3 : 0, event.type == "crush" ? -0.0001 : 0.3);
        }
        if (event.type == "pin") {
          require(!s.overnight, "events.pin", "pin requires a regular session");
          require(e.contains("strike"), "events.strike", "required field is missing");
          event.strike = number(e.at("strike"), "events.strike", 5400, 6600);
        }
        s.events.push_back(event);
      }
    }
    return s;
  } catch (const std::exception& error) {
    throw std::invalid_argument(file.string() + ": " + error.what());
  }
}

std::vector<Scenario> load_scenarios(const std::filesystem::path& user,
                                    const std::function<void(const std::string&)>& log) {
  auto out = builtin_scenarios();
  try {
    for (const auto& file : files(user)) {
      try {
        auto s = read_scenario(file);
        out.erase(std::remove_if(out.begin(), out.end(), [&](const auto& prior) { return prior.id == s.id; }), out.end());
        out.push_back(std::move(s));
      } catch (const std::exception& error) { if (log) log(error.what()); }
    }
  } catch (const std::exception& error) { if (log) log(user.string() + ": " + error.what()); }
  return out;
}
}  // namespace openport::providers
