// openportd: runs one market-data provider, the analytics engine and the web
// terminal's HTTP/WebSocket server.
//
//   openportd                                   # Cboe delayed SPX and SPY, no key needed
//   openportd --provider databento --symbols SPX,QQQ
//   openportd --provider massive --symbols SPY --poll-seconds 5
//
// API keys come from the environment (DATABENTO_API_KEY, MASSIVE_API_KEY) and never
// leave this machine except to authenticate with that provider.

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "openport/providers/factory.hpp"
#include "openport/server/api.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/web_server.hpp"

namespace {

using namespace openport;

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Settings {
  md::ProviderConfig provider{"cboe", "", {}};
  md::Subscription subscription{{"SPX", "SPY"}, 0, 0.0};
  std::string address = "127.0.0.1";
  unsigned short port = 8080;
  std::filesystem::path web_root;
  int threads = 2;
};

int usage(const char* error = nullptr) {
  if (error) std::fprintf(stderr, "openportd: %s\n\n", error);
  std::fprintf(stderr,
               "usage: openportd [--provider NAME] [--symbols SPX,SPY] [--address ADDR] [--port N]\n"
               "                 [--web-root DIR] [--expiries N] [--window F] [--poll-seconds N]\n"
               "                 [--option KEY=VALUE]...\n\n"
               "providers:");
  for (auto name : providers::provider_names()) {
    std::fprintf(stderr, " %.*s", static_cast<int>(name.size()), name.data());
  }
  std::fprintf(stderr,
               "\nkeys are read from the environment: DATABENTO_API_KEY, MASSIVE_API_KEY\n");
  return 2;
}

std::vector<std::string> split(const std::string& list) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t comma = list.find(',', start);
    std::string item = list.substr(start, comma - start);
    for (char& c : item) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!item.empty()) out.push_back(item);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

std::string env_key_for(std::string name) {
  for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const char* value = std::getenv((name + "_API_KEY").c_str());
  return value ? value : "";
}

/// The built web terminal: $OPENPORT_WEB_ROOT, next to the binary, or ./web/dist.
std::filesystem::path find_web_root(const char* argv0) {
  if (const char* env = std::getenv("OPENPORT_WEB_ROOT")) return env;
  std::error_code ec;
  const auto exe_dir = std::filesystem::weakly_canonical(argv0, ec).parent_path();
  for (const auto& candidate :
       {exe_dir / "web", exe_dir / "../../web/dist", exe_dir / "../share/openport/web",
        std::filesystem::current_path() / "web/dist"}) {
    if (std::filesystem::exists(candidate / "index.html", ec)) {
      return std::filesystem::weakly_canonical(candidate, ec);
    }
  }
  return std::filesystem::current_path() / "web/dist";
}

}  // namespace

int main(int argc, char** argv) {
  Settings settings;
  settings.web_root = find_web_root(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--help" || arg == "-h") return usage();
    if (!has_value) return usage(("missing value for " + arg).c_str());
    const std::string value = argv[++i];
    if (arg == "--provider") {
      settings.provider.name = value;
    } else if (arg == "--symbols") {
      settings.subscription.underlyings = split(value);
    } else if (arg == "--address") {
      settings.address = value;
    } else if (arg == "--port") {
      settings.port = static_cast<unsigned short>(std::stoi(value));
    } else if (arg == "--web-root") {
      settings.web_root = value;
    } else if (arg == "--expiries") {
      settings.subscription.max_expiries = std::stoi(value);
    } else if (arg == "--window") {
      settings.subscription.strike_window = std::stod(value);
    } else if (arg == "--poll-seconds") {
      settings.provider.options["poll_seconds"] = value;
    } else if (arg == "--option") {
      const std::size_t eq = value.find('=');
      if (eq == std::string::npos) return usage("--option takes KEY=VALUE");
      settings.provider.options[value.substr(0, eq)] = value.substr(eq + 1);
    } else {
      return usage(("unknown option " + arg).c_str());
    }
  }
  if (settings.subscription.underlyings.empty()) return usage("no symbols");
  settings.provider.api_key = env_key_for(settings.provider.name);

  std::unique_ptr<md::Provider> provider;
  try {
    provider = providers::make_provider(settings.provider);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "openportd: %s\n", error.what());
    return 1;
  }

  server::Engine engine(*provider, settings.subscription, {});
  server::WebServer web(settings.address, settings.port, settings.web_root,
                        [&engine](const server::ApiRequest& request) {
                          return server::handle_api(request, engine);
                        });
  try {
    web.start(settings.threads);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "openportd: cannot listen on %s:%u: %s\n", settings.address.c_str(),
                 settings.port, error.what());
    return 1;
  }
  engine.start();

  std::string symbols;
  for (const auto& symbol : settings.subscription.underlyings) {
    symbols += (symbols.empty() ? "" : ", ") + symbol;
  }
  std::printf("OpenPort on http://%s:%u  (provider %s: %s)\n", settings.address.c_str(), web.port(),
              settings.provider.name.c_str(), symbols.c_str());
  std::printf("web terminal: %s\n", settings.web_root.string().c_str());
  std::fflush(stdout);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    web.broadcast(server::tick_message(engine));
  }
  std::printf("\nshutting down\n");
  web.stop();
  engine.stop();
  return 0;
}
