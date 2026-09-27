#pragma once
#include <nlohmann/json.hpp>
#include "openport/analytics/chain_analytics.hpp"

namespace openport::server {
void validate_template(const nlohmann::json& value);
nlohmann::json template_chain(const analytics::UnderlyingMetrics& metrics, const analytics::SliceMetrics& slice);
/// The single leg picker used by the terminal and by playbooks. Throws clear
/// validation errors rather than substituting a nearby quoted contract.
nlohmann::json build_template(const nlohmann::json& value, const nlohmann::json& near,
                              const nlohmann::json& far = nullptr);
std::string template_tag(const nlohmann::json& value);
void strict_keys(const nlohmann::json& value, std::initializer_list<std::string_view> allowed);
double bounded_number(const nlohmann::json& value, double low, double high, std::string_view field);
}
