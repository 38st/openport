#include "openport/providers/factory.hpp"

#include <stdexcept>
#include <string>

#include "openport/providers/cboe.hpp"

namespace openport::providers {

std::vector<std::string_view> provider_names() { return {"cboe"}; }

std::unique_ptr<md::Provider> make_provider(const md::ProviderConfig& config) {
  if (config.name == "cboe") {
    CboeDelayedProvider::Options options;
    if (const auto it = config.options.find("poll_seconds"); it != config.options.end()) {
      options.poll_interval = std::chrono::seconds(std::stoi(it->second));
    }
    return std::make_unique<CboeDelayedProvider>(options);
  }
  throw std::invalid_argument("unknown provider: " + config.name);
}

}  // namespace openport::providers
