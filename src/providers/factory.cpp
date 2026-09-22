#include "openport/providers/factory.hpp"

#include <stdexcept>
#include <string>

#include "openport/providers/cboe.hpp"
#ifdef OPENPORT_WITH_DATABENTO
#include "openport/providers/databento.hpp"
#endif

namespace openport::providers {
namespace {

std::string option_or(const md::ProviderConfig& config, const std::string& key,
                      const std::string& fallback) {
  const auto it = config.options.find(key);
  return it == config.options.end() ? fallback : it->second;
}

}  // namespace

std::vector<std::string_view> provider_names() {
  return {
      "cboe",
#ifdef OPENPORT_WITH_DATABENTO
      "databento",
#endif
  };
}

std::unique_ptr<md::Provider> make_provider(const md::ProviderConfig& config) {
  if (config.name == "cboe") {
    CboeDelayedProvider::Options options;
    options.poll_interval = std::chrono::seconds(std::stoi(option_or(config, "poll_seconds", "15")));
    return std::make_unique<CboeDelayedProvider>(options);
  }
#ifdef OPENPORT_WITH_DATABENTO
  if (config.name == "databento") {
    DatabentoProvider::Options options;
    options.api_key = config.api_key;
    const std::string quotes = option_or(config, "quotes", "cbbo-1s");
    if (quotes == "cbbo-1s") {
      options.quotes = DatabentoProvider::QuoteSchema::Cbbo1s;
    } else if (quotes == "cmbp-1") {
      options.quotes = DatabentoProvider::QuoteSchema::Cmbp1;
    } else {
      throw std::invalid_argument("databento: quotes must be cbbo-1s or cmbp-1, not " + quotes);
    }
    options.trades = option_or(config, "trades", "on") != "off";
    return std::make_unique<DatabentoProvider>(std::move(options));
  }
#endif
  throw std::invalid_argument("unknown provider: " + config.name);
}

}  // namespace openport::providers
