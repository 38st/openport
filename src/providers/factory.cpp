#include "openport/providers/factory.hpp"

#include <stdexcept>
#include <string>

#include "openport/providers/cboe.hpp"
#include "openport/providers/massive.hpp"
#include "openport/providers/thetadata.hpp"
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

std::chrono::seconds seconds_option(const md::ProviderConfig& config, const std::string& key,
                                    std::chrono::seconds fallback) {
  const auto it = config.options.find(key);
  return it == config.options.end() ? fallback : std::chrono::seconds(std::stoi(it->second));
}

}  // namespace

std::vector<std::string_view> provider_names() {
  return {
      "cboe",
#ifdef OPENPORT_WITH_DATABENTO
      "databento",
#endif
      "massive",
      "thetadata",
  };
}

std::unique_ptr<md::Provider> make_provider(const md::ProviderConfig& config) {
  if (config.name == "cboe") {
    CboeDelayedProvider::Options options;
    options.poll_interval = seconds_option(config, "poll_seconds", options.poll_interval);
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
  if (config.name == "massive") {
    MassiveProvider::Options options;
    options.api_key = config.api_key;
    options.base_url = option_or(config, "base_url", options.base_url);
    options.poll_interval = seconds_option(config, "poll_seconds", options.poll_interval);
    return std::make_unique<MassiveProvider>(std::move(options));
  }
  if (config.name == "thetadata") {
    ThetaDataProvider::Options options;
    options.base_url = option_or(config, "base_url", options.base_url);
    options.poll_interval = seconds_option(config, "poll_seconds", options.poll_interval);
    return std::make_unique<ThetaDataProvider>(std::move(options));
  }
  throw std::invalid_argument("unknown provider: " + config.name);
}

}  // namespace openport::providers
