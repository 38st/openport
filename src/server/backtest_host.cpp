#include "openport/server/backtest.hpp"

#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace openport::server {
namespace {
using nlohmann::json;
bool run_id(std::string_view id) {
  return id.size() == 6 && std::all_of(id.begin(), id.end(), [](char value) { return value >= '0' && value <= '9'; });
}
json read_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot read saved backtest");
  return json::parse(input);
}
}
struct BacktestHost::Impl {
  Options options;
  std::vector<providers::Scenario> scenarios;
  std::mutex mutex;
  std::atomic_bool cancel{false};
  bool running = false;
  json current = nullptr;
  std::thread worker;
  explicit Impl(Options value) : options(std::move(value)), scenarios(providers::load_scenarios(options.scenario_dir)) {}
  ~Impl() { cancel = true; if (worker.joinable()) worker.join(); }
  json saved(const std::string& id, bool report) const {
    const auto directory = options.directory / id;
    auto state = read_json(directory / "state.json");
    if (!current.is_null() && current.at("id") == id) state = current;
    else if (state.at("status") == "running" || state.at("status") == "cancelling") state["status"] = "interrupted";
    if (report && std::filesystem::is_regular_file(directory / "report.json")) state["report"] = read_json(directory / "report.json");
    return state;
  }
};
BacktestHost::BacktestHost(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
BacktestHost::~BacktestHost() = default;
bool BacktestHost::handle(const ApiRequest& request, const MetricsSource& source, const ApiCompletion& complete) {
  const auto path = request.target.substr(0, request.target.find('?'));
  if (path != "/api/backtests" && !path.starts_with("/api/backtests/")) return false;
  auto& host = *impl_;
  const auto id = path == "/api/backtests" ? std::string{} : path.substr(15);
  if (!id.empty() && !run_id(id)) { complete(api_error(404, "NOT_FOUND", "Unknown backtest")); return true; }
  ApiResponse response;
  try {
    const std::lock_guard lock(host.mutex);
    if (request.method == "GET") {
      if (!id.empty()) {
        if (!std::filesystem::is_regular_file(host.options.directory / id / "state.json")) response = api_error(404, "NOT_FOUND", "Unknown backtest");
        else response.body = host.saved(id, true).dump();
      } else {
        json runs = json::array();
        std::vector<std::string> ids;
        if (std::filesystem::is_directory(host.options.directory))
          for (const auto& entry : std::filesystem::directory_iterator(host.options.directory))
            if (run_id(entry.path().filename().string()) && entry.is_directory() && std::filesystem::is_regular_file(entry.path() / "state.json")) ids.push_back(entry.path().filename().string());
        std::sort(ids.begin(), ids.end(), std::greater<>());
        for (const auto& item : ids) runs.push_back(host.saved(item, false));
        if (!host.current.is_null() && std::find(ids.begin(), ids.end(), host.current.at("id").get<std::string>()) == ids.end()) runs.insert(runs.begin(), host.current);
        response.body = json{{"runs", runs}, {"active", host.running ? host.current.at("id") : json(nullptr)}, {"label", kBacktestLabel}}.dump();
      }
    } else if (!host.options.enabled || source.status().trading.write == "disabled") {
      response = api_error(403, "WRITE_DISABLED", "Backtest writes are disabled");
    } else if (request.method == "DELETE") {
      if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
      if (!host.running || (!id.empty() && host.current.at("id") != id)) response = api_error(409, "BACKTEST_NOT_RUNNING", "No matching backtest is running");
      else {
        host.cancel = true;
        host.current["status"] = "cancelling";
        response.body = host.current.dump();
      }
    } else if (request.method == "POST" && id.empty()) {
      if (host.running) response = api_error(409, "BACKTEST_RUNNING", "A backtest is already running");
      else {
        const auto view = source.trading_view();
        if (!view || view->playbooks_json.empty()) throw std::invalid_argument("Playbooks are unavailable");
        BacktestRequest job;
        try { job = parse_backtest(json::parse(request.body), json::parse(view->playbooks_json), host.scenarios, host.options.recordings); }
        catch (const std::exception& error) { throw std::invalid_argument(error.what()); }
        job.analytics = host.options.analytics;
        job.dividends = host.options.dividends;
        job.actor = request.actor;
        if (host.options.directory.empty()) throw std::invalid_argument("Backtest storage is unavailable");
        std::filesystem::create_directories(host.options.directory);
        unsigned number = 1;
        for (const auto& entry : std::filesystem::directory_iterator(host.options.directory)) {
          const auto name = entry.path().filename().string();
          if (run_id(name)) number = std::max(number, static_cast<unsigned>(std::stoul(name)) + 1);
        }
        if (number > 999999) throw std::runtime_error("Backtest directory is full");
        std::ostringstream name;
        name << std::setfill('0') << std::setw(6) << number;
        const auto directory = host.options.directory / name.str();
        // run_backtest requires a new directory. Persist the initial state from its
        // first progress callback; GET also uses the in-memory state while starting.
        if (host.worker.joinable()) host.worker.join();
        host.cancel = false;
        host.current = {{"id", name.str()}, {"status", "running"}, {"phase", "days"}, {"completed", 0},
            {"total", job.days.size() * 2}, {"directory", std::filesystem::absolute(directory).string()}, {"label", kBacktestLabel}, {"report", nullptr}};
        host.running = true;
        try {
          host.worker = std::thread([&host, job = std::move(job), directory] {
            json report;
            try {
              report = run_backtest(job, directory, host.cancel, [&](std::size_t completed, std::string_view phase) {
                const std::lock_guard progress_lock(host.mutex);
                host.current["completed"] = completed;
                host.current["phase"] = phase;
                write_backtest_report(directory / "state.json", host.current);
              });
            } catch (const std::exception& error) {
              report = {{"status", "failed"}, {"message", error.what()}};
            }
            const std::lock_guard finished_lock(host.mutex);
            host.current["status"] = report.at("status");
            host.current["phase"] = "finished";
            if (report.contains("message")) host.current["error"] = report.at("message");
            try { write_backtest_report(directory / "state.json", host.current); }
            catch (const std::exception& error) { host.current["status"] = "failed"; host.current["error"] = error.what(); }
            host.running = false;
          });
        } catch (...) {
          host.running = false;
          host.current["status"] = "failed";
          throw;
        }
        response.status = 202;
        response.body = host.current.dump();
      }
    } else response = api_error(405, "METHOD_NOT_ALLOWED", "Unsupported backtest method");
    // A newly queued job may not have created its directory yet.
    if (request.method == "GET" && !id.empty() && !host.current.is_null() && host.current.at("id") == id && response.status == 404) {
      response.status = 200;
      response.body = host.current.dump();
    }
  } catch (const std::invalid_argument& error) { response = api_error(400, "INVALID_REQUEST", error.what()); }
  catch (const json::exception& error) { response = api_error(400, "INVALID_REQUEST", error.what()); }
  catch (const std::exception& error) { response = api_error(500, "BACKTEST_IO", error.what()); }
  complete(std::move(response));
  return true;
}
}  // namespace openport::server
