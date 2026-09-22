#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "openport/md/provider.hpp"

namespace openport::providers {

/// Names of the providers this build supports, e.g. {"cboe"}.
[[nodiscard]] std::vector<std::string_view> provider_names();

/// Creates a provider by name. Throws std::invalid_argument for an unknown name
/// or a missing API key.
[[nodiscard]] std::unique_ptr<md::Provider> make_provider(const md::ProviderConfig& config);

}  // namespace openport::providers
