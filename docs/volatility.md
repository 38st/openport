# Current volatility metrics

`GET /api/underlyings/{symbol}/volatility` computes current values from an immutable
analytics snapshot and the underlying's CandleStore. It returns market `as_of`,
`spot`, the front expiry's `forward`, `version`, and `mfiv`, `atm`, `skew`, `term`,
`realized`, `vrp`, `implied_moves` and a `sources` note for each part. Missing
numbers are null, with reasons where applicable. All reported volatility, RR, BF,
slope and curvature values are in **vol points**; variance is decimal annual
variance. A 20 vol-point value has variance 0.04. Percent moves are percentages,
not fractions. Ratios are dimensionless.

These calculations run on the requesting API worker, never in the engine's
once-a-second pass. They share the lazy SVI fits and per-snapshot lock used by the
surface route. Successes and missing results are cached by snapshot ownership,
so independent engines cannot share results even if symbol and version match.
History and event labels are read for that snapshot's first volatility request.
The next snapshot refreshes the results. There is no derived-metric history or
IV rank. The pure definitions live in `analytics/volatility.hpp`, `realized.hpp`
and their implementations, for reuse by a later series store.

## Model-free implied volatility

For each expiry, let F and D be the engine's parity forward and discount factor,
and T its ACT/365 time from market time to the settlement instant. K₀ is the
largest listed strike at or below F. Select puts below K₀ and calls above it;
at K₀ use the average of the call and put bid/ask mids. Both K₀ quotes must be
usable. In each wing exclude nonpositive/missing bids, nonpositive/missing asks,
crossed quotes and invalid adjusted mids. Stop after two consecutive excluded
strikes; one usable strike resets that count. The no-ask exclusion follows
Cboe's February 2025 methodology change.

For American options, subtract each known nonnegative `eep` from its mid.
`eep` reports `removed`, `partial`, `unavailable` or `not_applicable`. Missing
EEP leaves the raw mid and marks the result `proxy`. No exercise correction is
invented. Negative adjusted mids are unusable.

On the retained strikes, ΔK is half the distance between the two neighbours;
at each end it is the distance to the sole neighbour. Compute

```
variance = (2 Σ ΔK × Q(K) / (D × K²) − (F/K₀ − 1)²) / T
vol = 100 √variance
```

A negative or undefined variance is null. Each expiry reports `variance`, `vol`,
`k0`, `low`, `high`, `strikes`, `lower_stop`, `upper_stop`, `truncated`, `proxy`,
`eep` and `reason`. Stops are `zero_bids` (two exclusions, including absent asks)
or `window` (the subscribed chain ended first). Either window stop sets
`truncated`, including when it is the provider's full chain. The retained value
is still shown, with † in the terminal.

For 9, 30, 93, 182 and 365 calendar days, bracket the target in minutes from
market time, allowing only expiries at least 1,440 minutes away. Interpolate
**total variance**, then divide by target time:

```
variance(N) = [N₁ variance₁ (N₂ − N) + N₂ variance₂ (N − N₁)] / [(N₂ − N₁) N]
```

This is the Cboe time interpolation formula with N_year cancelling. An exact
listed maturity can stand alone. Outside the listed maturities, or with an
unusable bracketing expiry, return null and a reason; do not skip an unusable
bracket in search of a more distant one. SPX AM and SPXW PM expiries already
coexist in the underlying snapshot and both enter the calculation. Other
underlyings use all standard expiries. Bracket ids and truncation/proxy flags
are returned with each constant maturity.

This is **model-free implied volatility**, not Cboe's VIX or its other indices.
Differences include our parity-derived F and D, our available strike window,
all subscribed settlement families and bracketing expiries, our quote validation,
and estimated EEP corrections for American contracts. We do not reproduce Cboe's
index-specific expiry eligibility windows, rate inputs or index dissemination
rules. A finite strike strip can omit material tail variance.

## ATM and skew

At k = ln(K/F) = 0, use raw SVI only when admissibility, butterfly grid checks
and the snapshot's calendar checks pass. Both members of a flagged calendar pair
fall back to the observed smile. Otherwise interpolate smile IV linearly in k.
Return `source: svi` or `smile_interpolation`. Never extend either smile beyond
its observed/calibrated range. ATM constant maturities are 7, 30, 60, 90 and
180 calendar days, interpolated in total variance with no extrapolation.

For each 25Δ and 10Δ point, solve using that strike's own smile IV:

```
d₁ = −k / (σ(k) √T) + σ(k) √T / 2
call delta = Φ(d₁); put delta = Φ(d₁) − 1
RR = call vol − put vol
BF = (call vol + put vol)/2 − ATM vol
```

The convention is **Black-76 forward, premium-unadjusted, undiscounted**. It is
not the chain's spot delta. Scan the observed range to bracket each root, then
bisect. Unsupported delta points are null. At 30 days interpolate the call,
put and ATM points' total variances separately before computing RR and BF.
The 30-day delta-point strikes are null: no synthetic forward/strike is invented.

SVI ATM slope and curvature are analytic. With w = σ²T, they are
`100 σ w′/(2w)` and `100 σ [w″/(2w) − w′²/(4w²)]`. Interpolated smiles have no
reported derivatives. Term measures are MFIV 9d/30d and 30d/93d, plus ATM 30d
minus 7d. Missing or zero denominators give null ratios. ≈ marks interpolated
smiles, raw American mids, ATM move proxies and OHLC estimator fallbacks.

## Realized volatility and cones

Use completed regular sessions only, up to 2,600 daily bars. No future bar or
unfinished current daily bar enters daily RV. `daily_as_of` gives the last completed
daily bar actually available, shown in the terminal so stale history is visible. Windows are 5, 10, 21, 63, 126 and
252 consecutive sessions; N returns require N+1 closes. Parkinson and Garman–Klass
need only N OHLC bars; Yang–Zhang also needs the preceding close. Missing sessions or
nonpositive/nonfinite closes give null. Calendar continuity follows the `md`
calendar; before 2022 it knows weekdays only.

For N sessions, with r = ln(C/C_previous), o = ln(O/C_previous),
c = ln(C/O), h = ln(H/O), l = ln(L/O), the daily variance estimates are:

- Close-to-close: sample variance of r, demeaned, denominator N−1.
- Parkinson: mean of ln(H/L)² / (4 ln 2).
- Garman–Klass: mean of 0.5 ln(H/L)² − (2 ln 2−1)c².
- Yang–Zhang: sample variance of o + k × sample variance of c +
  (1−k) × mean[h(h−c) + l(l−c)], where k = 0.34 / (1.34 + (N+1)/(N−1)).

Report 100 √(252 × daily variance). Each OHLC estimator validates every bar it
uses: prices must be finite and positive and high/low must bracket open/close.
An invalid OHLC window uses its close-to-close estimate and reports `fallback`
and `invalid_ohlc_used_close_to_close`. The history parser and store retain
close-only daily rows for this purpose; candle charts still omit invalid OHLC.
Daily bars built from observed minutes inherit the coverage of those minutes.
They are not proof that every trade or opening print was received.

Cones use every valid rolling close-to-close estimate in the available history,
including the current completed window. Quantiles linearly interpolate ordered
observations at (count−1)p. Return min, p10, p25, p50, p75, p90, max, distinct
`days_used`, rolling `observations`, current RV and its percentile. The current
percentile is 100 × (count below + half the ties)/count. The chart adds ATM IV
interpolated to the close of the Nth coming business session, using the calendar
rather than converting sessions with 365/252. It remains null outside the chain.

Intraday RV uses complete regular-hours five-minute blocks built from minutes.
All five minutes must be present and finished at market `as_of`. Use consecutive
block closes, or a block's open for the first block and after a gap. Never include
an overnight or gap-spanning return. Sum squared log returns without demeaning,
multiply by session minutes / observed minutes and by 252, then take the square
root and multiply by 100. An early close has 210 rather than 390 minutes. Return
today's value and up to ten retained sessions, with observed minutes and a
`partial` flag. Annualising sparse delayed prints can be unreliable; coverage
is shown alongside the estimate.

VRP here is an ex-ante **volatility spread**: 30d MFIV minus 21-session
close-to-close RV, plus their ratio. It is not a forecast error or a difference
of squared variances. Missing inputs give null; zero RV makes only the ratio
null. MFIV truncation and EEP proxy flags carry through.

## Implied session moves and events

Use per-expiry MFIV total variance, or ATM total variance when MFIV is missing.
Subtract consecutive expiries' totals, starting with zero at market time. Divide
an interval's variance equally among its business sessions; a full early-close
session also counts as one. A partly elapsed regular session gets its remaining
fraction of that day's scheduled regular hours. Weekends and holidays get none.
An AM settlement's overnight-only increment belongs to its settlement session;
AM-to-PM variance adds to that same day's move. This is an allocation convention,
not an inferred overnight/intraday volatility pattern.

Report up to 15 coming sessions, one standard deviation in percent and points of
the interval's forward, with `shared`, `proxy` and `truncated` flags. Where AM and
PM intervals contribute to one date, sum the assigned variance and use the last
interval's forward. Count **all** sessions in an interval, including those beyond
the displayed 15. Null intervals stay missing. The `intervals` array also reports every expiry interval, including those beyond
the first 15 sessions, its forward variance and any missing-business-time reason.
Negative forward variance beyond
1e-12 is a calendar-arbitrage flag and produces a null session move. Roundoff
within that tolerance is zero. Today's remaining move uses the latest same-day
settlement's total variance when present, with its own flags. It is null without
a same-day expiry. A shared interval cannot isolate an event's premium.

`openportd --events FILE` reads supplied labels for every underlying:

```
YYYY-MM-DD,Label
```

Blank lines, `#` comments and an optional `date,label` header are allowed. Dates
must be valid and representable in the market timestamp range. Labels are trimmed,
1–160 bytes, with no control characters; commas after the first belong to the
label. Duplicate dates are rejected with a line number. Labels are read at startup
and only shown on matching sessions. No event dates ship with OpenPort.

## Comparing with published indices

On a machine with network access, run an unrestricted SPX chain during the session:

```bash
./build/apps/openport-probe cboe SPX --compare-mfiv --seconds 900
```

The comparison runs for the requested duration rather than stopping after the
first chain. It reads Cboe's chart endpoint through the existing history client:
`https://cdn-api.cboe.com/api/global/delayed_quotes/charts/intraday/_VIX.json`,
and `_VIX9D`, `_VIX3M`, `_VIX6M`, `_VIX1Y`. Daily files use `historical` instead
of `intraday`. Index names appear only as reference-series identifiers.

Cboe's intraday `datetime` is the New York minute at the bar's **close**. The
existing parser converts it to a start time; comparison restores the closing
minute and matches the snapshot's market minute exactly. Skip zero/invalid
bars and unmatched minutes; never substitute daily closes or nearest timestamps.
Keep the last sample per market minute so polling frequency cannot overweight it.
The summary gives matched samples, median |Δ|, p90 |Δ|, max |Δ| and median signed
Δ, where Δ is our value minus the index, in vol points. It also reports truncated
samples and 30d RR25/BF25 differences against OTM `vendor_iv` interpolation using
the same forward-delta convention. No vendor IV means no skew comparison sample.

A recording can be compared against saved intraday reference files without
network access:

```bash
./build/apps/openport-probe replay SPX --compare-mfiv --seconds 900 \
  --option file=/path/day.oprec --option speed=max --option loop=off \
  --comparison-dir /path/reference
```

That directory must contain `_VIX.json`, `_VIX9D.json`, `_VIX3M.json`,
`_VIX6M.json` and `_VIX1Y.json` in the intraday shape, covering the recording's
minutes. Without `--comparison-dir` the probe fetches the current chart files;
those will not match an older recording. Replay completion or provider failure
ends the run early. `nan` in probe statistics means no usable matched samples.

Tests use small synthetic JSON in the intraday and historical shapes, synthetic
option chains, exact minute-alignment cases and hand-computed statistics. No real
Cboe data files are committed. A live accuracy comparison has not been measured
for this feature. The README will carry measured numbers from a live session;
no accuracy claim is inferred from the synthetic tests.
