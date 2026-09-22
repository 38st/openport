# OpenPort

An open-source, self-hosted options platform. Plug in the market data provider you
already pay for and get live option chains, implied volatility and Greeks, volatility
surfaces, gamma and vanna exposure maps, paper trading and portfolio risk in a web
terminal.

Your API keys and your data stay on your machine: OpenPort runs locally under your own
data licence and never sends either anywhere else.

> Work in progress, built milestone by milestone. See the roadmap below.

## Providers

| Provider | Data | Key |
| --- | --- | --- |
| Cboe delayed quotes | US index and equity options, 15 minutes delayed | none |
| Databento | Real-time OPRA quotes, trades and open interest | yours |
| ThetaData | Real-time and historical options data | yours |
| Massive | Options snapshots and streaming quotes | yours |

Providers deliver very different things: Databento sends raw exchange quotes with no
Greeks, while others ship their own. OpenPort normalises every provider into the same
contracts, quotes, trades and open interest, then computes implied volatility and Greeks
itself, so the numbers on screen mean the same thing whichever provider you use. Where a
provider publishes its own Greeks, OpenPort shows them alongside for comparison.

## Roadmap

- [x] **Pricing core**: Black-76 and Black-Scholes-Merton with full Greeks (including
      vanna and volga), a safeguarded Newton implied-vol solver, and Cox-Ross-Rubinstein
      and Leisen-Reimer binomial trees for American exercise.
- [ ] **Provider layer**: normalised contracts, quotes, trades and open interest, with
      adapters for Cboe, Databento, ThetaData and Massive.
- [ ] **Chain analytics**: implied forwards from put-call parity, American implied
      volatility, SVI volatility surfaces, gamma and vanna exposure by strike.
- [ ] **Record and replay** of any provider's feed.
- [ ] **Paper trading and risk**: fills against live quotes, Greeks limits, scenario
      grids, P&L attribution and a kill switch.
- [ ] **Web terminal**
- [ ] **One-command install**, docs and benchmarks.

## Building

Needs CMake 3.25+, a C++20 compiler, OpenSSL 3 and Boost 1.83+. Other dependencies are
fetched and pinned by checksum at configure time.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
./build/bench/openport_bench
```

## License

MIT
