#include "openport/server/playbooks.hpp"
#include "strategy_template.hpp"
#include "../trading/state.hpp"
#include "openport/trading/history.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace openport::server {
namespace {
using nlohmann::json;
using namespace trading;
[[noreturn]] void invalid(const std::string& message) { throw std::invalid_argument(message); }
void text_field(const json& value, std::size_t maximum, std::string_view field, bool empty = false) {
  if (!value.is_string()) invalid(std::string(field) + " must be text");
  const auto text = value.get<std::string>();
  if ((!empty && text.empty()) || text.size() > maximum || std::any_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
    invalid(std::string(field) + " has invalid length or control characters");
}
int minute(const json& value) {
  if (!value.is_string()) invalid("Time must be HH:MM in New York");
  const auto text = value.get<std::string>();
  if (text.size() != 5 || text[2] != ':' || text[0] < '0' || text[0] > '2' || text[1] < '0' || text[1] > '9' || text[3] < '0' || text[3] > '5' || text[4] < '0' || text[4] > '9') invalid("Time must be HH:MM in New York");
  const int result = (text[0] - '0') * 600 + (text[1] - '0') * 60 + (text[3] - '0') * 10 + text[4] - '0';
  if (result >= 1440) invalid("Time must be HH:MM in New York");
  return result;
}
void integer(const json& value, int low, int high, std::string_view field) {
  if (!value.is_number_integer()) invalid(std::string(field) + " must be an integer");
  bounded_number(value, low, high, field);
}
void range(const json& value, double low, double high, std::string_view field) {
  strict_keys(value, {"min", "max"});
  const auto minimum = bounded_number(required(value, "min", std::string(field) + " min"), low, high, field);
  if (bounded_number(required(value, "max", std::string(field) + " max"), low, high, field) < minimum) invalid(std::string(field) + " min must not exceed max");
}
void direction(const json& value) {
  if (value != "above" && value != "below") invalid("direction must be above or below");
}
std::string key(std::string_view account, const std::string& id, const std::string& symbol) { return std::string(account) + ":" + id + ":" + symbol; }
std::uint64_t fingerprint(const std::string& text) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char ch : text) { hash ^= ch; hash *= 1099511628211ULL; }
  return hash;
}
const json* version_for(const json& catalogue, const std::string& tag) {
  if (!tag.starts_with("playbook:")) return nullptr;
  const auto split = tag.rfind("@v");
  if (split == std::string::npos) return nullptr;
  const auto id = tag.substr(9, split - 9);
  if (!catalogue.at("definitions").contains(id)) return nullptr;
  for (const auto& definition : catalogue.at("definitions").at(id).at("versions"))
    if (playbook_tag(definition) == tag) return &definition;
  return nullptr;
}
const json* order_definition(const json& catalogue, const Order& order) {
  for (const auto& tag : order.request.tags) if (const auto* definition = version_for(catalogue, tag)) return definition;
  return nullptr;
}
std::vector<Lifecycle> lives(const TradingView& view) {
  return lifecycles(view.snapshot->recent_fills, view.snapshot->closures, view.contracts);
}
bool tagged(const Order& order, const std::string& id) {
  return std::any_of(order.request.tags.begin(), order.request.tags.end(), [&](const auto& tag) { return tag.starts_with("playbook:" + id + "@v"); });
}
struct Trip {
  const Order* order = nullptr;
  std::vector<const Lifecycle*> legs;
  std::optional<md::Timestamp> closed;
  /// The order whose fill closed the trip, placing its close among the orders of
  /// that market second; zero when a settlement or reset closed it.
  OrderId closed_by = 0;
  Money net;
};
std::vector<Trip> trips(const TradingView& view, const std::vector<Lifecycle>& history) {
  std::map<OrderId, Trip> grouped;
  for (const auto& life : history) {
    const auto first = std::find_if(view.snapshot->recent_fills.begin(), view.snapshot->recent_fills.end(), [&](const auto& fill) { return fill.id == life.first_fill; });
    if (first == view.snapshot->recent_fills.end() || first->order_id == 0 || first->order_id > view.snapshot->recent_orders.size()) continue;
    auto& trip = grouped[first->order_id];
    trip.order = &view.snapshot->recent_orders[first->order_id - 1];
    trip.legs.push_back(&life);
    trip.net = trip.net + life.gross - life.fees;
  }
  std::vector<Trip> result;
  for (auto& [id, trip] : grouped) {
    (void)id;
    const auto count = multi_leg(trip.order->request) ? trip.order->request.legs.size() : 1;
    if (trip.legs.size() == count && std::all_of(trip.legs.begin(), trip.legs.end(), [](const auto* life) { return life->closed.has_value(); })) {
      trip.closed = 0;
      for (const auto* life : trip.legs) trip.closed = std::max(*trip.closed, *life->closed);
      std::uint64_t last = 0;
      bool settled = false;
      for (const auto* life : trip.legs) {
        if (*life->closed != *trip.closed) continue;
        if (life->closure || life->fills.empty()) settled = true;
        else last = std::max(last, life->fills.back());
      }
      const auto fill = std::find_if(view.snapshot->recent_fills.begin(), view.snapshot->recent_fills.end(), [&](const auto& item) { return item.id == last; });
      if (!settled && fill != view.snapshot->recent_fills.end()) trip.closed_by = fill->order_id;
    }
    result.push_back(std::move(trip));
  }
  return result;
}
bool guardrails_allow(const json& definition, const TradingView& view, md::Timestamp time, OrderId before = 0) {
  const auto id = definition.at("id").get<std::string>();
  int entries = 0;
  for (const auto& order : view.snapshot->recent_orders) {
    if (before != 0 && order.id >= before) break;
    if (order.id < view.snapshot->evaluation.first_order || order.parent || order.system || order.request.note.starts_with("Playbook automatic time stop") || !tagged(order, id)) continue;
    if (order.status == OrderStatus::Rejected || (order.status == OrderStatus::Cancelled && order.filled_quantity == 0)) continue;
    if (md::new_york_time(order.accepted_at).date == md::new_york_time(time).date) ++entries;
  }
  if (entries >= definition.at("guardrails").at("max_entries_per_day").get<int>()) return false;
  const auto history = lives(view);
  const auto cooldown = definition.at("guardrails").at("cooldown_minutes").get<int>() * md::kNanosPerMinute;
  // Scoring a past entry, a close in its own market second counts only if it came
  // first, as it did for the live check: orders are numbered as they arrive.
  const auto closed_first = [&](const Trip& trip) {
    return before ? *trip.closed < time || (*trip.closed == time && trip.closed_by < before) : *trip.closed <= time;
  };
  for (const auto& trip : trips(view, history))
    if (trip.order->id >= view.snapshot->evaluation.first_order && (!before || trip.order->id < before) && tagged(*trip.order, id) && trip.closed && closed_first(trip) &&
        trip.net < Money{} && time < *trip.closed + cooldown) return false;
  return true;
}
}
void validate_playbook(const json& definition) {
  strict_keys(definition, {"id", "version", "name", "description", "underlyings", "window", "conditions", "structure", "sizing", "management", "guardrails"});
  // A missing member is named, as the other refusals name their field.
  const auto field = [](const json& object, std::string_view key, std::string_view name = {}) -> const json& {
    return required(object, key, name.empty() ? key : name);
  };
  text_field(field(definition, "id"), 15, "id");
  const auto id = definition.at("id").get<std::string>();
  if (std::any_of(id.begin(), id.end(), [](char ch) { return !((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'); })) invalid("id must use lowercase letters, digits or hyphens");
  if (definition.contains("version")) integer(definition.at("version"), 1, 999999, "version");
  text_field(field(definition, "name"), 100, "name");
  text_field(field(definition, "description"), 2000, "description", true);
  const auto& underlyings = field(definition, "underlyings");
  if (!underlyings.is_array() || underlyings.empty() || underlyings.size() > 20) invalid("underlyings must contain 1–20 symbols");
  std::set<std::string> symbols;
  for (const auto& item : underlyings) {
    text_field(item, 12, "underlying");
    const auto symbol = item.get<std::string>();
    if (!symbols.insert(symbol).second || std::any_of(symbol.begin(), symbol.end(), [](char ch) { return !((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.'); })) invalid("underlyings must be unique uppercase symbols");
  }
  const auto& window = field(definition, "window");
  strict_keys(window, {"start", "end", "weekdays"});
  if (minute(field(window, "start", "window.start")) >= minute(field(window, "end", "window.end"))) invalid("Entry window start must precede end on the same New York day");
  const auto& weekdays = field(window, "weekdays", "window.weekdays");
  if (!weekdays.is_array() || weekdays.empty() || weekdays.size() > 5) invalid("weekdays must be a nonempty subset of 1–5 (Monday–Friday)");
  std::set<int> unique;
  for (const auto& day : weekdays) { integer(day, 1, 5, "weekday"); if (!unique.insert(day.get<int>()).second) invalid("Duplicate weekday"); }
  const auto& conditions = field(definition, "conditions");
  strict_keys(conditions, {"price", "iv_rank", "vrp_min", "term_inverted", "dte"});
  if (conditions.contains("price")) {
    const auto& price = conditions.at("price");
    strict_keys(price, {"reference", "direction", "value"});
    const auto& reference = field(price, "reference", "price reference");
    if (reference != "level" && reference != "prior_close" && reference != "day_open") invalid("price reference must be level, prior_close or day_open");
    direction(field(price, "direction", "price direction"));
    bounded_number(field(price, "value", "price value"), reference == "level" ? 0.000001 : -1000000, 1000000, "price value");
  }
  if (conditions.contains("iv_rank")) range(conditions.at("iv_rank"), -100, 100, "iv_rank");
  if (conditions.contains("vrp_min")) bounded_number(conditions.at("vrp_min"), -1000, 1000, "vrp_min");
  if (conditions.contains("term_inverted") && !conditions.at("term_inverted").is_boolean()) invalid("term_inverted must be boolean");
  if (conditions.contains("dte")) range(conditions.at("dte"), 0, 3650, "dte");
  const auto& structure = field(definition, "structure");
  strict_keys(structure, {"template", "expiry"});
  validate_template(field(structure, "template", "structure.template"));
  range(field(structure, "expiry", "structure.expiry"), 0, 3650, "expiry DTE");
  const auto& sizing = field(definition, "sizing");
  strict_keys(sizing, {"units", "floor_share"});
  if (sizing.size() != 1) invalid("Sizing requires either units or floor_share");
  if (sizing.contains("units")) integer(sizing.at("units"), 1, 100000, "units");
  else bounded_number(sizing.at("floor_share"), 0.000001, 1, "floor_share");
  const auto& management = field(definition, "management");
  strict_keys(management, {"take_profit_percent", "stop_credit_multiple", "stop_underlying", "close_by", "max_hold_days"});
  if (minute(field(management, "close_by", "management.close_by")) < minute(window.at("end"))) invalid("close_by must be at or after the entry window end");
  if (management.contains("take_profit_percent")) bounded_number(management.at("take_profit_percent"), 0.000001, 1000, "take_profit_percent");
  if (management.contains("stop_credit_multiple")) bounded_number(management.at("stop_credit_multiple"), 1.000001, 100, "stop_credit_multiple");
  if (management.contains("stop_underlying")) {
    if (management.contains("stop_credit_multiple")) invalid("Choose one stop rule");
    const auto& stop = management.at("stop_underlying");
    strict_keys(stop, {"level", "direction"});
    bounded_number(field(stop, "level", "underlying stop level"), 0.000001, 1000000, "underlying stop level");
    direction(field(stop, "direction", "underlying stop direction"));
  }
  if (management.contains("max_hold_days")) integer(management.at("max_hold_days"), 1, 365, "max_hold_days");
  const auto& guardrails = field(definition, "guardrails");
  strict_keys(guardrails, {"max_entries_per_day", "cooldown_minutes"});
  integer(field(guardrails, "max_entries_per_day", "guardrails.max_entries_per_day"), 1, 1000, "max_entries_per_day");
  integer(field(guardrails, "cooldown_minutes", "guardrails.cooldown_minutes"), 0, 525600, "cooldown_minutes");
}
bool playbook_window(const json& definition, md::Timestamp time) {
  const auto date = md::new_york_time(time);
  const int weekday = static_cast<int>((md::days_since_epoch(date.date) + 3) % 7) + 1;
  const auto& window = definition.at("window");
  const auto& weekdays = window.at("weekdays");
  const int current = date.seconds / 60;
  return std::find(weekdays.begin(), weekdays.end(), weekday) != weekdays.end() &&
      current >= minute(window.at("start")) && current < minute(window.at("end"));
}
bool playbook_conditions(const json& definition, const PlaybookInputs& inputs, double dte) {
  const auto& conditions = definition.at("conditions");
  const auto inside = [](double value, const json& limits) { return std::isfinite(value) && value >= limits.at("min").get<double>() && value <= limits.at("max").get<double>(); };
  if (conditions.contains("dte") && !inside(dte, conditions.at("dte"))) return false;
  if (conditions.contains("iv_rank") && !inside(inputs.iv_rank, conditions.at("iv_rank"))) return false;
  if (conditions.contains("vrp_min") && (!std::isfinite(inputs.vrp) || inputs.vrp <= conditions.at("vrp_min").get<double>())) return false;
  if (conditions.contains("term_inverted") && (!std::isfinite(inputs.term_ratio) || (inputs.term_ratio > 1) != conditions.at("term_inverted").get<bool>())) return false;
  if (conditions.contains("price")) {
    const auto& price = conditions.at("price");
    const double reference = price.at("reference") == "level" ? 0 : price.at("reference") == "prior_close" ? inputs.prior_close : inputs.day_open;
    const double level = reference + price.at("value").get<double>();
    if (!std::isfinite(inputs.spot) || !std::isfinite(level) || !(price.at("direction") == "above" ? inputs.spot > level : inputs.spot < level)) return false;
  }
  return true;
}
std::string playbook_tag(const json& definition) { return "playbook:" + definition.at("id").get<std::string>() + "@v" + std::to_string(definition.at("version").get<int>()); }
Money playbook_tick(const OrderRequest& order) {
  auto tick = Money::from_micros(100'000);  // The widest tier.
  for (const auto& symbol : order_symbols(order)) {
    // OSI symbols pad the root to six characters.
    auto root = symbol.substr(0, std::min<std::size_t>(6, symbol.size()));
    while (!root.empty() && root.back() == ' ') root.pop_back();
    tick = std::min(tick, tick_size(root, Money{}));
  }
  return tick;
}
Bracket playbook_bracket(const json& management, Money entry, Money tick) {
  Bracket bracket;
  // Match the existing ticket's percent-of-premium convention. Triggers take any cent;
  // the take-profit limit must sit on the legs' tick, and rounds to the better price
  // (pay less, or receive more), so it never asks less than the percent.
  const auto snap = [](double value) { return Money::from_micros(static_cast<std::int64_t>(std::round(value * 100)) * 10000); };
  const auto better = [&](double value) {
    return Money::from_micros(static_cast<std::int64_t>(std::floor(value / tick.dollars() + 1e-9)) * tick.micros());
  };
  if (management.contains("take_profit_percent")) bracket.take_profit = ExitSpec{{}, better(-entry.dollars() * management.at("take_profit_percent").get<double>() / 100)};
  if (management.contains("stop_credit_multiple")) {
    if (entry >= Money{}) invalid("A credit-multiple stop needs a credit entry");
    bracket.stop_loss = ExitSpec{Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, snap(-entry.dollars() * management.at("stop_credit_multiple").get<double>())}, {}};
  }
  if (management.contains("stop_underlying")) {
    const auto& stop = management.at("stop_underlying");
    bracket.stop_loss = ExitSpec{Trigger{TriggerSource::Underlying, stop.at("direction") == "below" ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove, Money::from_double(stop.at("level").get<double>())}, {}};
  }
  return bracket;
}
md::Timestamp playbook_deadline(const json& definition, md::Timestamp opened) {
  const auto& management = definition.at("management");
  auto date = md::new_york_time(opened).date;
  // Without a hold allowance the position closes the entry day. A hold allowance
  // moves close_by to that calendar day, clamped to the preceding business day.
  date = md::date_from_days(md::days_since_epoch(date) + management.value("max_hold_days", 0));
  while (!md::market_session(md::new_york_to_utc(date, 12, 0)).open) date = md::previous_business_day(date);
  const int close = std::min(minute(management.at("close_by")), md::regular_close_hour(date) * 60);
  return md::new_york_to_utc(date, close / 60, close % 60);
}
Playbooks::Playbooks(std::filesystem::path file, Json initial) : file_(std::move(file)) {
  if (!initial.is_null()) catalogue_ = std::move(initial);
  else if (!file_.empty() && std::filesystem::exists(file_)) {
    if (std::filesystem::file_size(file_) > 8 * 1024 * 1024) invalid("playbooks.json exceeds 8 MiB");
    std::ifstream input(file_);
    if (!input) invalid("Cannot read playbooks.json");
    catalogue_ = json::parse(input);
  }
  strict_keys(catalogue_, {"schema", "definitions", "modes"});
  if (catalogue_.at("schema") != 1 || !catalogue_.at("definitions").is_object() || !catalogue_.at("modes").is_object()) invalid("Invalid playbooks.json schema");
  for (const auto& [id, item] : catalogue_.at("definitions").items()) {
    strict_keys(item, {"versions", "deleted"});
    if (!item.at("versions").is_array() || item.at("versions").empty() || !item.at("deleted").is_boolean()) invalid("Invalid playbook history");
    int expected = 1;
    for (const auto& definition : item.at("versions")) {
      validate_playbook(definition);
      if (definition.at("id") != id || definition.at("version") != expected++) invalid("Invalid playbook version history");
    }
  }
  for (const auto& [account, modes] : catalogue_.at("modes").items()) {
    text_field(account, 100, "account");
    if (!modes.is_object()) invalid("Invalid account playbook modes");
    for (const auto& [id, mode] : modes.items())
      if (!catalogue_.at("definitions").contains(id) || (mode != "off" && mode != "stage" && mode != "auto")) invalid("Invalid saved playbook mode");
  }
}
void Playbooks::save(const Json& next) {
  if (next.dump().size() > 8 * 1024 * 1024) invalid("Playbook history exceeds 8 MiB");
  if (!file_.empty()) {
    const auto temporary = file_.string() + ".tmp";
    { std::ofstream output(temporary, std::ios::trunc); output << next.dump(2) << '\n'; output.flush(); if (!output) invalid("Cannot save playbooks.json"); }
    const int fd = ::open(temporary.c_str(), O_RDONLY);
    if (fd < 0) invalid("Cannot sync playbooks.json");
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) invalid("Cannot sync playbooks.json");
    std::filesystem::rename(temporary, file_);
  }
  catalogue_ = next;
  ++revision_;
}
Playbooks::Json Playbooks::change(const Json& command, std::string_view account, bool replay) {
  strict_keys(command, {"action", "definition", "id", "version", "mode", "staged"});
  const auto action = command.at("action").get<std::string>();
  if (action == "dismiss") {
    const auto stage_id = command.at("staged").get<std::string>();
    auto& list = staged_[std::string(account)];
    if (!list.is_array()) list = json::array();
    for (const auto& stage : list)
      if (stage.at("id") == stage_id)
        suppressed_[stage.at("key").get<std::string>()] = {md::new_york_time(stage.at("time").get<md::Timestamp>()).date, "Dismissed or sent for this day"};
    std::erase_if(list.get_ref<json::array_t&>(), [&](const auto& stage) { return stage.at("id") == stage_id; });
    ++revision_;
    return publication(account, replay);
  }
  auto next = catalogue_;
  auto& definitions = next.at("definitions");
  if (action == "create" || action == "update") {
    auto definition = command.at("definition");
    validate_playbook(definition);
    const auto id = definition.at("id").get<std::string>();
    if (action == "create") {
      if (definitions.contains(id)) invalid("Playbook ID already exists; old IDs cannot be reused");
      if (definitions.size() >= 100) invalid("At most 100 playbooks are supported");
      definition["version"] = 1;
      definitions[id] = {{"versions", json::array({definition})}, {"deleted", false}};
    } else {
      if (!definitions.contains(id) || definitions.at(id).at("deleted") == true) invalid("Unknown playbook");
      auto& versions = definitions[id]["versions"];
      const int current = versions.back().at("version").get<int>();
      if (!definition.contains("version") || definition.at("version") != current) invalid("Playbook changed; reload before editing");
      if (current >= 999999) invalid("Version limit reached");
      definition["version"] = current + 1;
      versions.push_back(definition);
    }
  } else if (action == "delete" || action == "mode") {
    const auto id = command.at("id").get<std::string>();
    if (!definitions.contains(id) || definitions.at(id).at("deleted") == true) invalid("Unknown playbook");
    if (action == "delete") {
      if (command.at("version") != definitions.at(id).at("versions").back().at("version")) invalid("Playbook changed; reload before deleting");
      definitions[id]["deleted"] = true;
    } else {
      const auto& value = command.at("mode");
      if (value != "off" && value != "stage" && value != "auto") invalid("Mode must be off, stage or auto");
      const auto mode = value.get<std::string>();
      if (mode == "auto" && !replay) invalid("Auto mode is only available on replay or scenario accounts; live-feed practice accounts cannot send automatically");
      next["modes"][std::string(account)][id] = mode;
    }
  } else invalid("Unknown playbook action");
  save(next);
  staged_.clear();
  return publication(account, replay);
}
bool Playbooks::enabled(std::string_view account) const {
  const auto& modes = catalogue_.at("modes");
  const auto found = modes.find(std::string(account));
  return found != modes.end() && std::any_of(found->begin(), found->end(), [](const auto& mode) { return mode != "off"; });
}
Playbooks::Json Playbooks::publication(std::string_view account, bool replay) const {
  const auto owner = std::string(account);
  json result = catalogue_;
  result["modes"] = catalogue_.at("modes").value(owner, json::object());
  result["staged"] = staged_.contains(owner) ? staged_.at(owner) : json::array();
  result["reasons"] = reasons_.contains(owner) ? reasons_.at(owner) : json::object();
  result["auto_allowed"] = replay;
  result["simulated"] = true;
  return result;
}
trading::OrderRequest Playbooks::take(const std::string& account, const std::string& staged) {
  auto& list = staged_[account];
  if (!list.is_array()) invalid("Staged order expired; refresh playbooks");
  for (auto iterator = list.begin(); iterator != list.end(); ++iterator) if (iterator->at("id") == staged) {
    auto order = iterator->at("request").get<OrderRequest>();
    list.erase(iterator);
    ++revision_;
    return order;
  }
  invalid("Staged order expired or changed; refresh playbooks");
}
void Playbooks::evaluate(const std::string& account, bool replay, md::Timestamp now,
    const std::map<std::string, std::shared_ptr<const analytics::UnderlyingMetrics>>& metrics,
    const TradingView& view, const std::function<PlaybookInputs(const std::string&, const Json&)>& inputs,
    const Preview& preview, const Send& send, const Cancel& cancel) {
  const TradingView* current = &view;
  std::shared_ptr<const TradingView> updated;
  // An automatic order publishes the account mid-evaluation, caching this
  // publication by revision; the new stages and reasons need a revision of their own.
  struct Revise {
    std::uint64_t& revision;
    ~Revise() { ++revision; }
  } revise{revision_};
  staged_[account] = json::array();
  reasons_[account] = json::object();
  const auto modes = catalogue_.at("modes").value(account, json::object());
  const auto history = lives(*current);
  const auto round_trips = trips(*current, history);
  if (replay) for (const auto& working : current->snapshot->open_orders) {
    const auto* definition = order_definition(catalogue_, working);
    if (definition && !working.parent && modes.value(definition->at("id").get<std::string>(), "off") == "auto" &&
        now >= playbook_deadline(*definition, working.accepted_at)) cancel(working.id);
  }
  if (replay) for (const auto& trip : round_trips) {
    const auto* definition = order_definition(catalogue_, *trip.order);
    if (!definition || trip.closed || trip.legs.empty() || modes.value(definition->at("id").get<std::string>(), "off") != "auto" ||
        now < playbook_deadline(*definition, trip.legs.front()->opened)) continue;
    OrderRequest close;
    close.client_order_id = "pb-close:" + std::to_string(trip.order->id) + ":" + std::to_string(now);
    close.type = OrderType::Market;
    close.tif = TimeInForce::Ioc;
    close.quantity = 1;
    close.tags = {playbook_tag(*definition)};
    // Actor hook: the Desk submission callback marks this command as automatic.
    close.note = "Playbook automatic time stop; entry " + std::to_string(trip.order->id);
    Quantity units = 0;
    for (const auto* life : trip.legs) if (life->quantity) units = std::gcd(units, std::abs(life->quantity));
    if (!units) continue;
    for (const auto* life : trip.legs) if (life->quantity)
      close.legs.push_back({life->symbol, life->quantity > 0 ? Side::Sell : Side::Buy, std::abs(life->quantity) / units});
    close.quantity = units;
    if (close.legs.size() == 1) {
      close.symbol = close.legs.front().symbol;
      close.side = close.legs.front().side;
      close.quantity *= close.legs.front().ratio;
      close.legs.clear();
    }
    const auto reply = send(close);
    if (reply.view) { updated = reply.view; current = updated.get(); }
  }
  for (const auto& [id, record] : catalogue_.at("definitions").items()) {
    const auto mode = modes.value(id, "off");
    if (record.at("deleted") == true || mode == "off" || (mode == "auto" && !replay)) continue;
    const auto& definition = record.at("versions").back();
    for (const auto& item : definition.at("underlyings")) {
      const auto symbol = item.get<std::string>();
      const auto stage_key = key(account, id, symbol);
      auto& reason = reasons_[account][id + ":" + symbol];
      try {
        if (!playbook_window(definition, now)) invalid(std::string(kOutsideEntryWindow));
        if (now >= playbook_deadline(definition, now)) invalid("Past the management deadline");
        if (const auto found = suppressed_.find(stage_key); found != suppressed_.end() && found->second.first == md::new_york_time(now).date)
          invalid(found->second.second);
        if (!guardrails_allow(definition, *current, now)) invalid("Playbook entry limit or loss cooldown");
        if (!metrics.contains(symbol)) invalid("Waiting for underlying data");
        const auto& underlying = *metrics.at(symbol);
        const auto& structure = definition.at("structure");
        const analytics::SliceMetrics* selected = nullptr;
        const analytics::SliceMetrics* far = nullptr;
        for (const auto& slice : underlying.slices) {
          const double dte = md::years_between(now, slice.expiry_time) * 365;
          if (slice.expiry_time > now && dte >= structure.at("expiry").at("min").get<double>() && dte <= structure.at("expiry").at("max").get<double>() &&
              (!selected || slice.expiry_time < selected->expiry_time)) selected = &slice;
          if (structure.at("template").contains("farExpiry") && template_chain(underlying, slice).at("expiry").at("id") == structure.at("template").at("farExpiry")) far = &slice;
        }
        if (!selected) invalid("No expiry in the requested DTE range");
        if (!playbook_conditions(definition, inputs(symbol, definition.at("conditions")), md::years_between(now, selected->expiry_time) * 365)) invalid("Entry conditions unmet or required data missing");
        const auto setup = build_template(structure.at("template"), template_chain(underlying, *selected), far ? template_chain(underlying, *far) : json(nullptr));
        OrderRequest order;
        order.type = OrderType::Limit;
        order.tif = TimeInForce::Gtc;
        order.quantity = 1;
        order.tags = {playbook_tag(definition), setup.at("tag").get<std::string>()};
        Money debit;
        for (const auto& leg : setup.at("legs")) {
          const auto contract = leg.at("symbol").get<std::string>();
          for (const auto& position : current->snapshot->positions)
            if (position.position.contract.osi_symbol() == contract && position.position.quantity) invalid("Selected contract is already held; playbooks require separate positions");
          for (const auto& working : current->snapshot->open_orders) for (const auto& pending_symbol : order_symbols(working.request))
            if (pending_symbol == contract) invalid("Selected contract already has a working order");
          const auto side = leg.at("side") == "buy" ? Side::Buy : Side::Sell;
          const auto ratio = leg.at("ratio").get<Quantity>();
          order.legs.push_back({contract, side, ratio});
          const auto premium = Money::from_double(leg.at("quote").at(side == Side::Buy ? "ask" : "bid").get<double>());
          debit = debit + premium * (side == Side::Buy ? ratio : -ratio);
        }
        // The net limit rounds toward paying more, one combo tick (a cent).
        debit = Money::from_micros(static_cast<std::int64_t>(std::ceil(debit.dollars() * 100 - 1e-8)) * 10000);
        order.limit_price = debit;
        const auto bracket = playbook_bracket(definition.at("management"), debit, playbook_tick(order));
        if (bracket.stop_loss || bracket.take_profit) order.bracket = bracket;
        order.client_order_id = "pb-preview:" + id + ":" + symbol;
        const auto& sizing = definition.at("sizing");
        const double share = sizing.value("floor_share", 1.0);
        if (sizing.contains("floor_share") && !current->snapshot->soft_floor && current->config.rules.max_drawdown <= Money{}) invalid("Floor-share sizing needs a plan or personal floor");
        const auto projection = preview(order, share);
        if (projection.max_units < 1) invalid(projection.decision.ok() ? "No units fit buying power, limits and floor room" : projection.decision.message);
        order.quantity = sizing.contains("units") ? std::min(projection.max_units, sizing.at("units").get<Quantity>()) : projection.max_units;
        const auto final_preview = preview(order, share);
        if (!final_preview.decision.ok()) invalid(final_preview.decision.message);
        order.note = json{{"playbook", playbook_tag(definition)}, {"max_units", projection.max_units}, {"floor_share", share},
            {"close_by", md::format_timestamp(playbook_deadline(definition, now))},
            {"automatic", mode == "auto"}, {"max_loss", final_preview.max_loss ? json(final_preview.max_loss->str()) : json(nullptr)}}.dump();
        // The stage keeps its ID while the same contracts are selected, so a send
        // clicked on a moving market still finds it; the price and size it sends are
        // the latest, and the normal order checks run again. The client order ID is
        // new at each account version, so a later entry is never taken for a retry.
        std::string contracts = stage_key + "@v" + std::to_string(definition.at("version").get<int>()) + "|" + md::format_date(md::new_york_time(now).date);
        for (const auto& leg : order.legs)
          contracts += "|" + leg.symbol + (leg.side == Side::Buy ? "+" : "-") + std::to_string(leg.ratio);
        const auto stage_id = std::to_string(fingerprint(contracts));
        order.client_order_id = "playbook:" + stage_id + ":" + std::to_string(current->snapshot->account_version);
        json stage{{"id", stage_id}, {"key", stage_key}, {"playbook", id}, {"version", definition.at("version")},
            {"name", definition.at("name")}, {"underlying", symbol}, {"time", now}, {"request", order},
            {"legs", setup.at("legs")}, {"units", order.quantity}, {"net", debit.str()},
            {"max_loss", final_preview.max_loss ? json(final_preview.max_loss->str()) : json(nullptr)},
            {"max_loss_basis", final_preview.max_loss_basis}, {"close_by", md::format_timestamp(playbook_deadline(definition, now))},
            {"management", definition.at("management")}, {"simulated", true}};
        reason = "Ready";
        if (mode == "auto") {
          const auto reply = send(order);
          if (reply.view) { updated = reply.view; current = updated.get(); }
          if (!reply.decision.ok() || !reply.error_code.empty()) reason = reply.decision.message;
          else reason = "Automatic order sent";
          // Failed automatic attempts do not flood the journal on every update; the
          // refusal stays the setup's reason for the rest of the day.
          if (!reply.decision.ok() || !reply.error_code.empty())
            suppressed_[stage_key] = {md::new_york_time(now).date, "Automatic entry refused for this day: " + reply.decision.message};
        } else staged_[account].push_back(std::move(stage));
      } catch (const std::exception& error) { reason = error.what(); }
    }
  }
}

nlohmann::json playbook_report(const json& catalogue, const TradingView& view) {
  const auto history = lives(view);
  const auto round_trips = trips(view, history);
  json reports = json::object();
  for (const auto& [id, record] : catalogue.at("definitions").items()) {
    (void)record;
    json rows = json::array();
    for (const auto& trip : round_trips) {
      if (!tagged(*trip.order, id) || trip.order->id < view.snapshot->evaluation.first_order) continue;
      const auto* definition = order_definition(catalogue, *trip.order);
      if (!definition || trip.legs.empty()) continue;
      const auto& order = *trip.order;
      const auto opened = trip.legs.front()->opened;
      bool window = true;
      for (const auto& fill : view.snapshot->recent_fills) if (fill.order_id == order.id) window = window && playbook_window(*definition, fill.time);
      bool size = false;
      const auto risk = view.snapshot->strategy_reviews.find(std::to_string(order.id));
      const auto planned = risk == view.snapshot->strategy_reviews.end() ? std::optional<Money>{} : risk->second.planned_risk;
      const auto& sizing = definition->at("sizing");
      if (sizing.contains("units")) {
        const auto cap = sizing.at("units").get<Quantity>();
        size = order.request.quantity <= cap;
        for (const auto* life : trip.legs) {
          Quantity ratio = 1;
          for (const auto& leg : order.request.legs) if (leg.symbol == life->symbol) ratio = leg.ratio;
          size = size && life->max_quantity <= cap * ratio;
        }
      }
      else if (planned && trip.legs.front()->entry_context && trip.legs.front()->entry_context->floor_room) {
        Money fees;
        for (const auto& fill : view.snapshot->recent_fills) if (fill.order_id == order.id) fees = fees + fill.fee;
        const auto room = *trip.legs.front()->entry_context->floor_room;
        const auto share = static_cast<std::int64_t>(std::floor(sizing.at("floor_share").get<double>() * 1000000));
        size = *planned + fees < room && *planned + fees <= room.prorate(share, 1000000);
      }
      if (sizing.contains("floor_share") && order.request.client_order_id.starts_with("playbook:")) {
        const auto evidence = json::parse(order.request.note, nullptr, false);
        if (evidence.is_object() && evidence.contains("playbook") && evidence.at("playbook") == playbook_tag(*definition) &&
            evidence.contains("max_units") && evidence.at("max_units").is_number_integer() &&
            evidence.contains("floor_share") && evidence.at("floor_share") == sizing.at("floor_share")) {
          const auto cap = evidence.at("max_units").get<Quantity>();
          size = cap > 0 && order.request.quantity <= cap;
          for (const auto* life : trip.legs) {
            Quantity ratio = 1;
            for (const auto& leg : order.request.legs) if (leg.symbol == life->symbol) ratio = leg.ratio;
            size = size && life->max_quantity / ratio <= cap && life->max_quantity % ratio == 0;
          }
        }
      }
      bool exits = false;
      try {
        Money net;
        for (const auto* life : trip.legs) net = net + life->open_notional * life->direction;
        const auto entry = order.request.limit_price.value_or(net.prorate(1, std::max<Quantity>(1, order.filled_quantity)));
        const auto required = playbook_bracket(definition->at("management"), entry, playbook_tick(order.request));
        const auto actual = order.request.bracket.value_or(Bracket{});
        exits = actual == required;
      } catch (const std::exception&) {}
      const bool timely = trip.closed && *trip.closed <= playbook_deadline(*definition, opened);
      // As the live check did, when the entry arrived.
      const bool guarded = guardrails_allow(*definition, view, order.accepted_at, order.id);
      json rules{{"entry_window", window}, {"size", size}, {"exits", exits},
          {"time_stop", trip.closed || view.snapshot->time >= playbook_deadline(*definition, opened) ? json(timely) : json(nullptr)},
          {"guardrails", guarded}};
      bool followed = true;
      int passed = 0, measured = 0;
      for (const auto& value : rules) if (value.is_boolean()) { ++measured; if (value.get<bool>()) ++passed; else followed = false; }
      rows.push_back({{"order", std::to_string(order.id)}, {"version", definition->at("version")}, {"tag", playbook_tag(*definition)},
          {"opened", md::format_timestamp(opened)}, {"closed", trip.closed ? json(md::format_timestamp(*trip.closed)) : json(nullptr)},
          {"net", trip.net.str()}, {"rules", rules}, {"passed_rules", passed}, {"measured_rules", measured}, {"followed", followed},
          {"r", trip.closed && planned && *planned > Money{} ? json(trip.net.dollars() / planned->dollars()) : json(nullptr)}});
    }
    const auto stats = [&](int filter) {
      int count = 0, wins = 0, losses = 0, r_count = 0, passed = 0, measured = 0;
      Money total, win_total, loss_total;
      double r_total = 0;
      for (const auto& row : rows) {
        if (filter >= 0 && row.at("followed").get<bool>() != (filter == 1)) continue;
        passed += row.at("passed_rules").get<int>(); measured += row.at("measured_rules").get<int>();
        if (row.at("closed").is_null()) continue;
        ++count;
        const auto net = Money::parse(row.at("net").get<std::string>());
        total = total + net;
        if (net > Money{}) { ++wins; win_total = win_total + net; }
        if (net < Money{}) { ++losses; loss_total = loss_total - net; }
        if (row.at("r").is_number()) { ++r_count; r_total += row.at("r").get<double>(); }
      }
      return json{{"trades", count}, {"win_rate", count ? json(static_cast<double>(wins) / count) : json(nullptr)},
          {"average_win", wins ? json(win_total.prorate(1, wins).str()) : json(nullptr)},
          {"average_loss", losses ? json((-loss_total).prorate(1, losses).str()) : json(nullptr)},
          {"expectancy", count ? json(total.prorate(1, count).str()) : json(nullptr)},
          {"profit_factor", loss_total > Money{} ? json(win_total.dollars() / loss_total.dollars()) : json(nullptr)},
          {"no_losses", wins > 0 && losses == 0}, {"average_r", r_count ? json(r_total / r_count) : json(nullptr)},
          {"adherence", measured ? json(static_cast<double>(passed) / measured) : json(nullptr)}};
    };
    reports[id] = {{"trades", rows}, {"all", stats(-1)}, {"followed", stats(1)}, {"deviated", stats(0)}};
  }
  return reports;
}
}  // namespace openport::server
