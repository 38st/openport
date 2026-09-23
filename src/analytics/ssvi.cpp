#include "openport/analytics/ssvi.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>

#include "svi_data.hpp"

namespace openport::analytics {
namespace {
using Vec = std::array<double, 3>;
struct ExpiryData {
  std::size_t index;
  double years, theta;
  std::vector<detail::SviDatum> points;
};
struct Candidate {
  Vec x{};  // rho, eta*(1+|rho|)/2, gamma
  double loss = std::numeric_limits<double>::infinity();
  bool converged = false;
};

SsviParameters parameters(const Vec& x) { return {x[0], 2 * x[1] / (1 + std::abs(x[0])), x[2]}; }

double phi(const SsviParameters& p, double theta) {
  return p.eta * std::exp(-p.gamma * std::log(theta) - (1 - p.gamma) * std::log1p(theta));
}

double variance(double rho, double phi_k, double theta) {
  return theta / 2 * (1 + rho * phi_k + std::hypot(phi_k + rho, std::sqrt(1 - rho * rho)));
}

Candidate evaluate(Vec x, const std::vector<ExpiryData>& data) {
  x[0] = std::clamp(x[0], -1 + 1e-7, 1 - 1e-7);
  x[1] = std::clamp(x[1], 1e-8, 1.0);
  x[2] = std::clamp(x[2], 1e-6, .5);
  Candidate candidate{x, 0, false};
  const auto p = parameters(x);
  for (const auto& expiry : data) {
    const double f = phi(p, expiry.theta);
    for (const auto& point : expiry.points) {
      const double error = variance(p.rho, f * point.k, expiry.theta) - point.w;
      candidate.loss += point.weight * error * error;
    }
  }
  return candidate;
}

Candidate refine(const Candidate& seed, const std::vector<ExpiryData>& data) {
  std::array<Candidate, 4> simplex{seed, seed, seed, seed};
  for (std::size_t j = 0; j < 3; ++j) {
    auto x = seed.x;
    x[j] += j == 2 && x[j] > .4 ? -.05 : .05;
    simplex[j + 1] = evaluate(x, data);
  }
  for (int iteration = 0; iteration < 600; ++iteration) {
    std::stable_sort(simplex.begin(), simplex.end(), [](const auto& a, const auto& b) { return a.loss < b.loss; });
    double width = 0;
    for (std::size_t i = 1; i < 4; ++i)
      for (std::size_t j = 0; j < 3; ++j)
        width = std::max(width, std::abs(simplex[i].x[j] - simplex[0].x[j]));
    if (width < 1e-9 && std::isfinite(simplex[0].loss)) {
      simplex[0].converged = true;
      return simplex[0];
    }
    Vec center{};
    for (std::size_t i = 0; i < 3; ++i)
      for (std::size_t j = 0; j < 3; ++j) center[j] += simplex[i].x[j] / 3;
    auto at = [&](double scale) {
      Vec x{};
      for (std::size_t j = 0; j < 3; ++j) x[j] = center[j] + scale * (center[j] - simplex[3].x[j]);
      return evaluate(x, data);
    };
    const auto reflected = at(1);
    if (reflected.loss < simplex[0].loss) {
      const auto expanded = at(2);
      simplex[3] = expanded.loss < reflected.loss ? expanded : reflected;
    } else if (reflected.loss < simplex[2].loss) {
      simplex[3] = reflected;
    } else {
      const bool outside = reflected.loss < simplex[3].loss;
      const auto contracted = at(outside ? .5 : -.5);
      if (contracted.loss < (outside ? reflected.loss : simplex[3].loss)) simplex[3] = contracted;
      else {
        for (std::size_t i = 1; i < 4; ++i) {
          Vec x{};
          for (std::size_t j = 0; j < 3; ++j) x[j] = (simplex[0].x[j] + simplex[i].x[j]) / 2;
          simplex[i] = evaluate(x, data);
        }
      }
    }
  }
  return {};  // Never report an unconverged fit as successful.
}

// Average duplicate ATM log strikes deterministically before interpolation.
double atm_iv(const std::vector<detail::SviDatum>& data) {
  std::vector<std::pair<double, double>> knots;
  for (std::size_t i = 0; i < data.size();) {
    const double k = data[i].k;
    double sum = 0;
    std::size_t count = 0;
    do { sum += data[i].iv; ++count; ++i; } while (i < data.size() && data[i].k == k);
    knots.emplace_back(k, sum / static_cast<double>(count));
  }
  if (knots.empty() || knots.front().first > 0 || knots.back().first < 0) return kNaN;
  const auto hi = std::lower_bound(knots.begin(), knots.end(), 0.0,
      [](const auto& point, double k) { return point.first < k; });
  if (hi->first == 0) return hi->second;
  const auto& lo = *(hi - 1);
  return std::lerp(lo.second, hi->second, -lo.first / (hi->first - lo.first));
}

void isotonic(std::vector<ExpiryData>& data, bool& adjusted) {
  struct Pool { std::size_t begin, end; double sum, weight; };
  std::vector<Pool> pools;
  for (std::size_t i = 0; i < data.size();) {
    Pool pool{i, i, 0, 0};
    do { pool.sum += data[i].theta; ++pool.weight; ++i; }
    while (i < data.size() && data[i].years == data[pool.begin].years);
    pool.end = i;
    pools.push_back(pool);
    while (pools.size() > 1) {
      auto& left = pools[pools.size() - 2];
      const auto right = pools.back();
      if (left.sum / left.weight <= right.sum / right.weight) break;
      left.end = right.end; left.sum += right.sum; left.weight += right.weight;
      pools.pop_back();
    }
  }
  for (const auto& pool : pools) {
    const double theta = pool.sum / pool.weight;
    for (std::size_t i = pool.begin; i < pool.end; ++i) {
      adjusted = adjusted || std::abs(theta - data[i].theta) > 1e-14 * data[i].theta;
      data[i].theta = theta;
    }
  }
}
}  // namespace

bool ssvi_admissible(const SsviParameters& p) {
  return std::isfinite(p.rho) && std::isfinite(p.eta) && std::isfinite(p.gamma) &&
         std::abs(p.rho) < 1 && p.eta > 0 && p.gamma > 0 && p.gamma <= .5 &&
         p.eta * (1 + std::abs(p.rho)) <= 2 + 1e-14;
}

double ssvi_variance(const SsviParameters& p, double k, double theta) {
  if (!(theta > 0) || !std::isfinite(theta) || !std::isfinite(k) || !ssvi_admissible(p)) return kNaN;
  return variance(p.rho, phi(p, theta) * k, theta);
}

double ssvi_iv(const SsviParameters& p, double k, double theta, double years) {
  return years > 0 && std::isfinite(years) ? std::sqrt(ssvi_variance(p, k, theta) / years) : kNaN;
}

SviParameters ssvi_slice(const SsviParameters& p, double theta) {
  if (!(theta > 0) || !std::isfinite(theta) || !ssvi_admissible(p)) return {};
  const double f = phi(p, theta);
  return {theta / 2 * (1 - p.rho * p.rho), theta * f / 2, p.rho,
          -p.rho / f, std::sqrt(1 - p.rho * p.rho) / f};
}

SsviFit fit_ssvi(std::span<const SsviSlice> slices) {
  const auto start = std::chrono::steady_clock::now();
  SsviFit fit;
  fit.expiries.resize(slices.size());
  auto finish = [&] {
    fit.fit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return fit;
  };
  std::vector<ExpiryData> data;
  for (std::size_t i = 0; i < slices.size(); ++i) {
    auto& expiry = fit.expiries[i];
    const auto& slice = slices[i];
    if (!(slice.years > 0) || !std::isfinite(slice.years)) {
      expiry.reason = "time to expiry must be finite and positive";
      continue;
    }
    auto points = detail::prepare_svi(slice.points, slice.years);
    expiry.points = points.size();
    std::size_t distinct = 0;
    for (std::size_t j = 0; j < points.size(); ++j)
      if (j == 0 || points[j].k != points[j - 1].k) ++distinct;
    if (distinct < 5) {
      expiry.reason = "need at least five distinct two-sided OTM IV points";
      continue;
    }
    const double atm = atm_iv(points);
    const double theta = atm * atm * slice.years;
    if (!(theta > 0) || !std::isfinite(theta)) {
      expiry.reason = "eligible quotes must bracket ATM (k=0)";
      continue;
    }
    expiry.min_k = points.front().k;
    expiry.max_k = points.back().k;
    if (expiry.max_k - expiry.min_k < 1e-5 || !std::isfinite(expiry.max_k - expiry.min_k)) {
      expiry.reason = "log-moneyness range is too narrow or non-finite";
      continue;
    }
    data.push_back({i, slice.years, theta, std::move(points)});
  }
  std::stable_sort(data.begin(), data.end(), [](const auto& a, const auto& b) {
    return a.years < b.years;
  });
  if (data.size() < 2 || data.front().years == data.back().years) {
    fit.status = SviStatus::TooFewPoints;
    fit.reason = "need at least two usable distinct tenors with ATM quotes";
    return finish();
  }
  isotonic(data, fit.monotone_adjusted);
  std::vector<Candidate> seeds;
  for (double rho : {-.8, -.4, 0.0, .4, .8})
    for (double scale : {.15, .4, .8})
      for (double gamma : {.15, .35, .5}) seeds.push_back(evaluate({rho, scale, gamma}, data));
  std::stable_sort(seeds.begin(), seeds.end(), [](const auto& a, const auto& b) { return a.loss < b.loss; });
  Candidate best;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto candidate = refine(seeds[i], data);
    if (candidate.converged && candidate.loss < best.loss) best = candidate;
  }
  if (!std::isfinite(best.loss) || !ssvi_admissible(parameters(best.x))) {
    fit.reason = "constrained surface least squares did not converge";
    return finish();
  }
  fit.parameters = parameters(best.x);
  double total = 0;
  for (const auto& expiry : data) {
    double error = 0;
    for (const auto& point : expiry.points) {
      const double residual = ssvi_iv(fit.parameters, point.k, expiry.theta, expiry.years) - point.iv;
      error += point.weight * residual * residual;
    }
    auto& result = fit.expiries[expiry.index];
    result.theta = expiry.theta;
    result.rmse_vol_points = 100 * std::sqrt(error);
    total += error;
  }
  fit.rmse_vol_points = 100 * std::sqrt(total / static_cast<double>(data.size()));
  fit.status = SviStatus::Ok;
  return finish();
}

SsviFit fit_ssvi(std::span<const SliceMetrics> slices) {
  const auto start = std::chrono::steady_clock::now();
  std::vector<SsviSlice> input;
  for (const auto& slice : slices) input.push_back({slice.years, detail::svi_points(slice)});
  auto fit = fit_ssvi(input);
  fit.fit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return fit;
}
}  // namespace openport::analytics
