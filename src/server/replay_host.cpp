#include "openport/server/replay_host.hpp"

#include <algorithm>
#include <atomic>
#include <nlohmann/json.hpp>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "openport/providers/demo.hpp"
#include "openport/providers/replay.hpp"
#include "openport/server/plans.hpp"

namespace openport::server {
namespace {
using nlohmann::json;

/// A plain file name in the recordings directory: no directories, no hidden files.
bool plain_name(std::string_view name) {
  return !name.empty() && name.size() <= 255 && name.front() != '.' &&
         name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos;
}

json header_json(const md::RecordingHeader& header) {
  return {{"provider", header.provider}, {"symbols", header.subscription.underlyings},
          {"started", header.started > 0 ? json(md::format_timestamp(header.started)) : json(nullptr)},
          {"delay_seconds", header.capabilities.delay.count()}};
}

json recordings_json(const std::filesystem::path& directory) {
  json list = json::array();
  std::error_code ec;
  if (directory.empty() || !std::filesystem::is_directory(directory, ec)) return list;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    if (entry.is_regular_file(ec) && plain_name(entry.path().filename().string())) files.push_back(entry.path());
  // Recordings are named by when they started: newest first.
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.filename() > b.filename(); });
  for (const auto& file : files) {
    json item{{"file", file.filename().string()}, {"bytes", std::filesystem::file_size(file, ec)}};
    try {
      const md::RecordingReader reader(file);
      item.update(header_json(reader.header()));
    } catch (const std::exception& error) {
      item["error"] = error.what();
    }
    list.push_back(std::move(item));
  }
  return list;
}

ApiResponse ok(const json& body, int status = 200) { return {status, body.dump()}; }

json parse_body(const ApiRequest& request, std::initializer_list<std::string_view> allowed) {
  if (request.body.size() > 64 * 1024) throw std::invalid_argument("Body exceeds 64 KiB");
  auto body = request.body.empty() ? json::object() : json::parse(request.body);
  if (!body.is_object()) throw std::invalid_argument("Expected a JSON object");
  for (auto it = body.begin(); it != body.end(); ++it)
    if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
      throw std::invalid_argument("Unknown field: " + it.key());
  return body;
}

json demo_json() {
  const providers::DemoOptions demo;
  return {{"provider", providers::kDemoProvider}, {"symbols", {"SPX", "SPY"}},
          {"started", md::format_timestamp(md::new_york_to_utc(demo.date, 9, 30))}};
}

/// Generates the demo day where only this process looks, and returns a player for
/// it. The player holds the file open, so it is unlinked at once and leaves nothing behind.
std::unique_ptr<providers::ReplayProvider> demo_provider(int speed) {
  static std::atomic<unsigned> count{0};
  const auto path = std::filesystem::temp_directory_path() /
                    ("openport-demo-" + std::to_string(::getpid()) + "-" + std::to_string(++count) + ".oprec");
  std::error_code ec;
  try {
    providers::write_demo_recording(path);
    auto provider = std::make_unique<providers::ReplayProvider>(providers::ReplayProvider::Options{path, speed, false, nullptr});
    std::filesystem::remove(path, ec);
    return provider;
  } catch (...) {
    std::filesystem::remove(path, ec);
    throw;
  }
}

int speed_field(const json& body) {
  const auto& value = body.at("speed");
  if (!value.is_number_integer() || !providers::ReplayProvider::valid_speed(value.get<int>()))
    throw std::invalid_argument("speed must be 0 (as fast as possible), 1, 2, 5, 10, 30, 60, 120 or 300");
  return value.get<int>();
}
}  // namespace

struct ReplayHost::Session {
  std::string file;
  bool demo = false;
  // The engine reads the provider, so it is declared after it and stops first.
  std::unique_ptr<providers::ReplayProvider> provider;
  std::unique_ptr<Engine> engine;

  [[nodiscard]] json state() const {
    const auto time = provider->time();
    json out = header_json(provider->header());
    out["file"] = file;
    out["demo"] = demo;
    out["speed"] = provider->speed();
    out["paused"] = provider->paused();
    out["finished"] = provider->finished();
    out["time"] = time > 0 ? json(md::format_timestamp(time)) : json(nullptr);
    return out;
  }
};

ReplayHost::ReplayHost(Options options) : options_(std::move(options)) {}
ReplayHost::~ReplayHost() { stop(); }

std::shared_ptr<ReplayHost::Session> ReplayHost::current() const {
  const std::lock_guard lock(mutex_);
  return session_;
}

void ReplayHost::stop() {
  std::shared_ptr<Session> old;
  {
    const std::lock_guard lock(mutex_);
    old = std::move(session_);
  }
  // Requests still in flight keep a replay alive until they complete.
  old.reset();
}

bool ReplayHost::handle(const ApiRequest& request, const ApiCompletion& complete) {
  constexpr std::string_view prefix = "/api/replay";
  const std::string_view target = request.target;
  if (!target.starts_with(prefix)) return false;
  const auto rest = target.substr(prefix.size());
  if (rest.empty() || rest.front() == '?') {
    control(request, complete);
    return true;
  }
  if (rest.front() != '/') return false;
  const auto session = current();
  if (!session) {
    complete(api_error(404, "NO_REPLAY", "No replay is running; start one at /api/replay"));
    return true;
  }
  ApiRequest forwarded = request;
  forwarded.target = "/api" + std::string(rest);
  handle_api_async(forwarded, *session->engine, [session, complete](ApiResponse response) { complete(std::move(response)); });
  return true;
}

void ReplayHost::control(const ApiRequest& request, const ApiCompletion& complete) {
  if (request.target != "/api/replay") {
    complete(api_error(400, "INVALID_REQUEST", "/api/replay takes no query parameters"));
    return;
  }
  try {
    if (request.method == "GET") {
      const auto session = current();
      complete(ok({{"directory", options_.recordings.string()}, {"recordings", recordings_json(options_.recordings)},
                   {"demo", options_.demo ? demo_json() : json(nullptr)},
                   {"replay", session ? session->state() : json(nullptr)}}));
    } else if (request.method == "POST") {
      const auto body = parse_body(request, {"file", "demo", "speed", "plan"});
      if (body.contains("demo") && !body.at("demo").is_boolean()) throw std::invalid_argument("demo must be true or false");
      const bool demo = body.contains("demo") && body.at("demo").get<bool>();
      if (demo == body.contains("file")) throw std::invalid_argument("Give either file, a recording's name, or demo: true");
      if (demo && !options_.demo) {
        complete(api_error(404, "NOT_FOUND", "The demo market is off on this server"));
        return;
      }
      std::string name = "Demo market";
      std::filesystem::path path;
      if (!demo) {
        if (!body.at("file").is_string()) throw std::invalid_argument("file must be a recording's name");
        name = body.at("file").get<std::string>();
        path = options_.recordings / name;
        std::error_code ec;
        if (options_.recordings.empty() || !plain_name(name) || !std::filesystem::is_regular_file(path, ec)) {
          complete(api_error(404, "NOT_FOUND", "No recording " + name + " in the recordings directory"));
          return;
        }
      }
      const int speed = body.contains("speed") ? speed_field(body) : 1;
      const auto* plan = find_plan(body.contains("plan") && body.at("plan").is_string() ? body.at("plan").get<std::string>() : "practice");
      if (!plan || !plan->unlocked_by.empty()) throw std::invalid_argument("plan must be an evaluation or practice plan; see GET /api/plans");
      auto session = std::make_shared<Session>();
      session->file = name;
      session->demo = demo;
      session->provider = demo ? demo_provider(speed)
          : std::make_unique<providers::ReplayProvider>(providers::ReplayProvider::Options{path, speed, false, nullptr});
      auto engine = options_.engine;
      engine.paper_journal.clear();
      engine.paper_accounts.clear();
      engine.paper_sink.reset();
      engine.record_file.clear();
      engine.candles = std::make_shared<CandleStore>();
      engine.paper.rules = plan->rules;
      engine.paper.initial_cash = plan->initial_cash;
      // The replay's clock is the recording's: sessions and feed checks see that day.
      auto* provider = session->provider.get();
      engine.clock = [provider] { return provider->time(); };
      const auto& header = provider->header();
      session->engine = std::make_unique<Engine>(*provider, md::Subscription{header.subscription.underlyings, 0, 0.0}, engine);
      session->engine->start();
      std::shared_ptr<Session> old;
      {
        const std::lock_guard lock(mutex_);
        old = std::exchange(session_, session);
      }
      old.reset();
      complete(ok({{"replay", session->state()}}, 201));
    } else if (request.method == "PUT") {
      const auto body = parse_body(request, {"speed", "paused", "skip"});
      const auto session = current();
      if (!session) {
        complete(api_error(404, "NO_REPLAY", "No replay is running"));
        return;
      }
      if (body.contains("paused") && !body.at("paused").is_boolean()) throw std::invalid_argument("paused must be true or false");
      if (body.contains("skip") && !body.at("skip").is_boolean()) throw std::invalid_argument("skip must be true or false");
      if (body.contains("speed")) session->provider->set_speed(speed_field(body));
      if (body.contains("paused")) session->provider->set_paused(body.at("paused").get<bool>());
      if (body.contains("skip") && body.at("skip").get<bool>()) session->provider->skip();
      complete(ok({{"replay", session->state()}}));
    } else if (request.method == "DELETE") {
      stop();
      complete(ok({{"replay", nullptr}}));
    } else {
      complete(api_error(405, "METHOD_NOT_ALLOWED", "Use GET, POST, PUT or DELETE"));
    }
  } catch (const json::exception& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const std::invalid_argument& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const std::exception& error) {
    complete(api_error(422, "REPLAY_FAILED", error.what()));
  }
}

std::string ReplayHost::tick() const {
  const auto session = current();
  if (!session) return {};
  auto message = json::parse(tick_message(*session->engine));
  message["type"] = "replay_tick";
  message["replay"] = session->state();
  return message.dump();
}

}  // namespace openport::server
