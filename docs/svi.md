# SVI volatility surface

OpenPort fits raw SVI independently to each expiry's out-of-the-money smile:

\[
k=\log(K/F),\qquad w(k)=a+b\{\rho(k-m)+\sqrt{(k-m)^2+\sigma^2}\},
\qquad IV(k)=\sqrt{w(k)/T}.
\]

Here **w is total variance**, T is years, and IV is decimal annual volatility.
These conventions follow Gatheral's raw SVI formulation; they matter when applying
wing bounds. The fit is descriptive: it does not repair arbitrage or interpolate
between expiries.

## Calibration

Use the chain's existing smile IV and the corresponding OTM side (call for K≥F,
put otherwise). Require finite positive bid, finite ask≥bid, and positive finite
bid IV≤smile IV≤ask IV. Missing, crossed, zero-bid and invalid IV markets do not
enter calibration. At least five distinct log-strikes and a range ≥1e-5 are needed.
Fitting uses the entire eligible expiry, independently of the API display window;
the existing market-display filter still hides price spreads above half the mid.

For IV spread s, initial weight is 1/max(s, 0.0001)². Cap it at four times the upper median
weight, then lower the common cap until no observation has more than 25% of the
normalized total weight. The spread floor is 0.01 vol points. Minimize weighted
**total-variance** squared residuals against IV²T. Report
100√Σ weight·(fitted IV−market IV)² as RMSE in vol points; it is a diagnostic in
vol units, not the minimized objective.

Following Zeliade's dimension reduction, fix (m,σ), put y=(k−m)/σ, c=bσ and d=ρc.
Then w=a+dy+c√(1+y²), a three-variable constrained least-squares problem. A primal
active-set quadratic solver handles the linear bounds. The curved minimum-variance
constraint is enforced by adding supporting planes at its analytic minimizer;
this allows negative a, unlike restricting a≥0. Only residuals ≤1e-12 in total
variance are rounded onto an active boundary. The inner solve has finite iteration
limits and rejects nonconvergence.

The outer search evaluates 20 fixed seeds: m at the range midpoint plus
{−0.5,−0.25,0,0.25,0.5} times the range, and σ at {0.05,0.15,0.4,1} times the range.
Refine the best four using two-dimensional Nelder–Mead in (m,log σ), at most 400
iterations each. Search bounds are midpoint±2 ranges for m and [1e-4,5] ranges for
σ. Convergence requires simplex widths below 1e-8 ranges in m and 1e-8 in log σ.
This is a deterministic bounded local search, not a global optimality guarantee.
Flat smiles can have non-identifiable parameters. Failure returns no parameters,
an explicit reason, eligible point count and measured time.

## Constraints and diagnostics

Enforce b≥0, |ρ|≤1−1e-7, σ>0 and a+bσ√(1−ρ²)≥0. The wing limit is
b(1+|ρ|)≤min(2,4/T), with numerical admissibility tolerance 1e-12.
**Lee's total-variance wing bound is 2, not 4/T.** The latter is retained as the
additional requested cap; for T>2 it is more restrictive than Lee's bound.
See [Lee's moment formula](https://math.uchicago.edu/~rl/moment.pdf).

With x=k−m and h=√(x²+σ²), derivatives are w′=b(ρ+x/h), w″=bσ²/h³. Evaluate

\[
g(k)=\left(1-\frac{kw'}{2w}\right)^2-
\frac{(w')^2}{4}\left(\frac1w+\frac14\right)+\frac{w''}{2}
\]

on 2,001 uniformly spaced k values, including both calibration endpoints. Report
the minimum and its k. Any negative g fails; undefined density, including zero w,
also fails. For calendars, sort successful fits by T and compare consecutive pairs
across all settlement families on 2,001 points over the intersection of their
calibration ranges; skip disjoint ranges. At each k, the later expiry's IV increase
needed to remove a crossing is `100 * (sqrt(w_earlier / T_later) - sqrt(w_later / T_later))`
vol points. Flag a pair only when this increase exceeds `max(0.1, RMSE_earlier,
RMSE_later)` vol points. Report the largest increase and its k, without judging
either fit's extrapolation beyond its quotes.

These are **finite-grid diagnostics**, not a global no-arbitrage certificate:
violations between grid points or outside the checked range can be missed, and
wing conditions alone do not ensure a valid density. Fits remain unchanged when a
check fails. The UI says “Grid checks pass”, never “arbitrage-free”.

## API and display

`/api/underlyings/{symbol}/surface` retains existing fields. Each expiry adds
`svi` with `a,b,rho,m,sigma,rmse_vol_points,points,status,reason,fit_ms,`
`butterfly_min_g,butterfly_k,butterfly_ok`; `points[].svi_iv` is decimal fitted IV.
A failed fit has `svi: null` and null point estimates. Expiry-level `svi_status`,
`svi_reason`, `svi_points`, `svi_fit_ms` preserve failure information.
`svi_years,svi_min_k,svi_max_k` support sampling without rounded-tenor errors.
Parameters retain full precision. Top-level `calendar_violations` contains
`{earlier,later,k,vol_points}` using existing expiry IDs; it covers only returned
expiries. `vol_points` is the full later-expiry IV increase, not the excess over
tolerance. The table shows the size, k and counterpart expiry for both members.

Fitting runs only in the surface API. A per-snapshot mutex coalesces concurrent
requests; the engine never takes it. Cache successes and failures for each
immutable (symbol, metrics version) snapshot, using weak snapshot ownership to
isolate different sources and remove expired entries on subsequent requests.
Larger expiry prefixes fit only the additional expiries. Changing the display
window reuses fits; calendar comparisons and JSON serialization still run.

The Volatility view offers Market / SVI / Both, draws market dots and 241 curve
samples uniform in k (also on the strike axis), and clips curves to the requested
window and calibration range. Each shown expiry has parameters, RMSE, point count
and explicit butterfly/calendar or failure labels. Parameter cells wrap on narrow
screens and colors use the existing light/dark theme tokens.

## Validation and timing

Tests cover exact parameter recovery including negative a; bounded recovery under
small deterministic noise; short and long tenors; active constraints and flat
smiles; invalid, missing and degenerate inputs; capped quote weighting; determinism;
flat and known safe densities; Vogt's published butterfly counterexample; calendar
ordering; API fields, nulls, concurrent cache reuse, windows, prefixes and version
invalidation; and web sampling, toggles and diagnostic rendering.

Run the repeatable synthetic workload with `./build/bench/openport_measure_svi`.
It uses 250 OTM points per expiry, deterministic small IV noise, varying quote
spreads, 60 tenors from 2 days to about 2 years, five warmups and 25 timed samples.
It is SPX-sized, **not a live SPX calibration accuracy measurement**. Local Release
build timings (ms):

| Workload | Median | p90 |
| --- | ---: | ---: |
| One fit, 250 points, including butterfly check | 1.487 | 1.848 |
| 60 fits plus butterfly and calendar checks | 89.984 | 90.783 |
| Full API, cold fits and 15,000 serialized market points | 133.998 | 138.867 |
| Full API, cached fits, same points and calendar checks | 43.037 | 50.637 |

No time is added to the engine analytics pass. The first surface response pays
calibration cost on its API worker; repeated responses still pay serialization cost.

## References

- Gatheral (2004), *A parsimonious arbitrage-free implied volatility parameterization
  with application to the valuation of volatility derivatives*, Global Derivatives,
  Madrid; [bibliographic entry in Gatheral & Jacquier](https://arxiv.org/html/1204.0646#bib.bib13).
- Zeliade Systems (2009; revised 2012),
  [*Quasi-Explicit Calibration of Gatheral's SVI model*](https://www.zeliade.com/wp-content/uploads/whitepapers/zwp-0005-SVICalibration.pdf).
  OpenPort uses the dimension reduction with weighted observations and the full
  nonnegative-minimum constraint, rather than the paper's simpler nonnegative-a box.
- Gatheral & Jacquier (2014),
  [*Arbitrage-free SVI volatility surfaces*](https://arxiv.org/abs/1204.0646),
  Quantitative Finance 14(1), 59–71. Density diagnostic: Lemma 2.2; Axel Vogt example:
  a=−0.0410, b=0.1331, ρ=0.3060, m=0.3586, σ=0.4153.
