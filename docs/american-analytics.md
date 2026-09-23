# American rates and de-Americanisation (A3)

American call/put parity does not identify a discount rate: the strike-dependent
put early-exercise premium changes its slope. European expiries retain the
existing robust parity fit. Expiries shorter than 30 days borrow the median of
that underlying's fitted longer rates; absent those, they use `fallback_rate`.

After European analytics, the engine constructs zero-rate curves from the
expiries with their own successful rate fit. At least two distinct positive
tenors are required. Zero rates interpolate linearly in time and extrapolate
flat. The latest eligible curve supplies American discounts; SPX takes priority.
All European books are processed before American books in each pass. Changing
the selected curve also refreshes American analytics with unchanged quotes.
Mixed exercise-style books use only European slices to construct curves.

An American expiry never fits a parity slope, including with de-Americanisation
disabled. It takes its rate from the supplied curve, otherwise the flat assumed
rate (`openportd --rate R`, default `0.04`, permitted range `[-0.05, 0.25]`).

## Correction

1. With the fixed discount, estimate F0 from the weighted median of
   `K + (C-P)/D`, using the existing spread-weight caps and MAD rejection.
2. Solve first-pass Black-76 mid IVs. Set `r = -log(D)/T` and
   `q = r - log(F0/S)/T`, using the common reference spot. Below
   `min_days_for_rate` (30 days), use the median implied q from this underlying's
   longer live expiries, falling back to own q if no longer tenor is available.
   Clamp tree q to **[-5%, 20%]**: a modelling safeguard covering negative carry
   and high dividend/borrow yields while limiting amplified timing noise.
3. Evaluate American minus European prices on the **same 31-step Leisen-Reimer
   lattice**, at the same volatility and carry. American value uses rollback;
   European value uses the binomial terminal probabilities and the same terminal
   payoffs. Sum weights outward from their mode and normalize, avoiding tail
   underflow and the cost of a second rollback. No analytic BSM price is
   subtracted from a lattice price.
4. Refit F from premium-adjusted near-ATM parity with the existing robust
   fixed-discount estimator. Solve final Black-76 mid/bid/ask IVs from adjusted
   prices, while keeping **displayed bid/ask/mid as the raw market quotes** and
   preserving receipt flags. The API exposes `eep` separately: null when not
   computed, zero when the computed premium is zero. Positive EEP is explained
   in the web IV-cell tooltip.

The implementation corrects all two-sided options with a usable first-pass IV,
including both parity legs, all OTM smile inputs, and ITM fallback smile inputs.
If an ITM quote cannot imply its own first-pass IV, its partner's valid first-pass
IV supplies the premium estimate. It skips a tree exactly when exercise cannot
help: calls with q <= 0 and r >= 0, or puts with r <= 0 and q >= 0. Premiums are
reused for parity and all three quote prices within the pass; there is no cache
across changing market inputs. A negative premium-adjusted bid has no bid IV,
but does not prevent a positive adjusted mid from having an IV when the original
quote was two-sided. A received zero bid retains the no-mid-IV behavior.

An underlying print more than 30 minutes older than the option data
(`max_spot_age_minutes`, above the 16:00/16:15 ETF close gap) is treated
as absent, regardless of provider: parity supplies the reference spot and
`spot_source` becomes `parity`. This prevents stale closing index levels from
anchoring live overnight Greeks. Session clocks are documented in
[runtime.md](runtime.md#product-sessions-and-cboe-clocks).

For a one-day synthetic American chain with a 0.05% spot mismatch, longer-tenor
carry reduces the maximum OTM IV error from 0.070276 to 0.002375 vol points for
an upward spot mismatch, and from 0.035256 to 0.004235 for a downward mismatch.
The test uses 1,001-step quotes at S=100, r=4.5%, q=1%, vol=20%, strikes 97..103
in increments of 0.25 and a one-year companion expiry.

One correction iteration suffices for the measured acceptance grid below. This
is a continuous-yield approximation, not a discrete cash-dividend exercise
model. The numerical tolerances below are measured on that grid, not guaranteed
for every volatility, dividend or maturity. Greeks remain European Black-76
Greeks at the final smile IV, accurate on the OTM side of American contracts.

Exposure counts every side with received usable OI at a finite-smile-IV strike,
even when that side's own IV failed. `priced` still counts own-IV successes;
`oi_coverage` is receipt coverage over the exposure set. The web coverage warning
uses missing quotes or OI, not missing IVs.

## Accuracy measured on this Mac

S=100, r=4.5%, q=1%, vol=20%, T in {0.25, 1.0}, K=80..120 in increments of 2.5,
both calls and puts. Compare EEP with the same-tree difference at 2,001 steps:

| LR steps | Max absolute EEP error, all sides | Max error, OTM sides |
| ---: | ---: | ---: |
| 15 | $0.07360427 | $0.02229692 |
| 21 | $0.06245326 | $0.01643721 |
| **31** | **$0.03776984** | **$0.01137375** |
| 51 | $0.02133947 | $0.00589251 |
| 71 | $0.01481348 | $0.00410468 |
| 101 | $0.01050770 | $0.00275754 |

For the end-to-end check, generate American quotes with the existing 1,001-step
LR pricer and small symmetric spreads, then supply a flat 4.5% SPX curve:

| T | D fixed, no correction: F error | Corrected F error | Max corrected OTM IV error |
| ---: | ---: | ---: | ---: |
| 0.25 | -0.050555% | +0.001102% | 0.016711 vol points |
| 1.00 | -0.419232% | -0.027016% | 0.086979 vol points |

Both forward residuals are inside 0.05% and both IV residuals inside 0.1 vol
points. The regression also verifies that raw American parity misestimates the
rate by more than 50 bp. The paired-pricer test independently checks the terminal
expectation against a full European rollback on the same 31-step tree.

## Timing

Release build on this Mac, 2026-09-22. Deterministic SPY-sized chain: 12,028
options, 31 expiries at 1+i² days (i=0..30), S=600, 194 strikes per expiry from
120 to 1085 in $5 increments. Quotes are generated once with 101-step American
LR at r=4.5%, q=1%, vol=20%. Timing includes the complete `analyze()` call,
including forward fits, bid/mid/ask IVs, Greeks, exposures and gamma flip. It
excludes fixture construction. Five warm-up passes precede 50 measured passes.
The new paths use a flat 4.5% SPX curve.

| Version | Median | p90 | Own-IV successes |
| --- | ---: | ---: | ---: |
| Before A3, original binary and defaults | 19.704 ms | 24.234 ms | 9,565 |
| A3, curve supplied, correction disabled | 22.333 ms | 28.825 ms | 11,850 |
| A3, curve supplied, 31-step correction | 36.292 ms | 43.007 ms | 10,811 |

The final median is **1.84×** the original pass, within the approximately 2×
budget. Correct discounts change which IV solves succeed, so the baseline and
new passes do not have identical successful-solve counts. A preliminary
51-step implementation with two rollbacks took 50.823 ms (assumed-rate path);
31 steps met the acceptance tolerances, and the terminal expectation removed
unnecessary European rollback work. No ITM-side omission or approximate EEP
interpolation was needed.

Reproduce the new timing and accuracy probe after building the core:

```sh
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cmake --build build -j12
c++ -std=c++20 -O3 -DNDEBUG -Iinclude bench/measure_american.cpp \
  build/src/libopenport_core.a -o /tmp/openport-american-measure
/tmp/openport-american-measure
/tmp/openport-american-measure --no-deamericanize
/tmp/openport-american-measure --accuracy
```

The original baseline executable was built before editing the analytics and
retained as `/tmp/openport-a3-before` for the paired timing run. The first baseline
measurement was 20.013 ms; the final paired run above was 19.704 ms. Both used the
same fixture and five-plus-fifty pass methodology.
