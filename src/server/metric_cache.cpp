#include "metric_cache.hpp"

namespace openport::server {
std::shared_ptr<SurfaceFits> surface_cache(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  using Key = std::weak_ptr<const analytics::UnderlyingMetrics>;
  static std::mutex mutex;
  static std::map<Key, std::shared_ptr<SurfaceFits>, std::owner_less<Key>> cache;
  const std::lock_guard lock(mutex);
  std::erase_if(cache, [](const auto& entry) { return entry.first.expired(); });
  auto& entry = cache[Key(metrics)];
  if (!entry) entry = std::make_shared<SurfaceFits>();
  return entry;
}

analytics::VolatilityMetrics cached_volatility(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  const auto cache = surface_cache(metrics);
  const std::lock_guard lock(cache->mutex);
  if (!cache->volatility) {
    while (cache->fits.size() < metrics->slices.size())
      cache->fits.push_back(analytics::fit_svi(metrics->slices[cache->fits.size()]));
    cache->volatility = analytics::volatility_metrics(*metrics, cache->fits);
  }
  return *cache->volatility;
}
}  // namespace openport::server
