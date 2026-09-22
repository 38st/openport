# OpenPort

An open-source options engine in C++20: pricing, implied volatility, volatility
surfaces, an order book and matching engine, paper trading against live markets,
and real-time portfolio risk, with a web terminal on top.

> Work in progress. Built milestone by milestone; see the roadmap below.

## Roadmap

- [x] **Pricing core**: Black-76 and Black-Scholes-Merton with full Greeks (including
      vanna and volga), a safeguarded Newton implied-vol solver, Cox-Ross-Rubinstein
      and Leisen-Reimer binomial trees for American exercise, and Deribit's
      coin-settled option conventions.
- [ ] **Volatility surface**: SVI fits per expiry, static-arbitrage checks
- [ ] **Order book and matching engine**: price-time priority, benchmarked
- [ ] **Market data**: Deribit (live BTC options) and Cboe (delayed SPX options)
- [ ] **Paper trading, risk and challenge mode**: realistic fills, Greeks limits,
      scenario grids, P&L attribution, a tamper-evident audit log
- [ ] **Web terminal**
- [ ] **Docs, benchmarks and deployment**

## Building

Needs CMake 3.25+, a C++20 compiler, OpenSSL 3 and Boost 1.83+. Other
dependencies are fetched and pinned by checksum at configure time.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
./build/bench/openport_bench
```

## License

MIT
