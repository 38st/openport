# Contributing

Bug reports, fixes and features are welcome. For anything larger than a fix, open an
issue first to agree on the approach.

## Building and testing

You need CMake 3.25+, a C++20 compiler (CI uses GCC 13 and Apple Clang), Boost 1.83+, OpenSSL 3,
zlib, zstd and Node 22+. Other dependencies are fetched and pinned by checksum.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPENPORT_WERROR=ON
cmake --build build -j
(cd build && ctest -j 8)                 # C++ tests
cd web && npm ci && npx tsc -p tsconfig.json && npx vitest run && npm run build
```

`cd web && npm run dev` serves the terminal on :5173 against an `openportd` on :8080.
CI runs the same checks on every push and pull request, with GCC on Ubuntu, Apple
Clang on macOS and a Docker smoke test, all with warnings as errors.

## Python and API contract

The Python package uses Python 3.10+. Its tests need pytest, jsonschema, pyyaml,
pandas, websockets and MCP 2.x. With the prepared development environment:

```sh
.venv-py/bin/python -m pytest python/tests
.venv-py/bin/python tools/contract_test.py http://127.0.0.1:8080 --token "$OPENPORT_WRITE_TOKEN" --spec docs/openapi.yaml
```

Python tests run a threaded synthetic HTTP server, with local socket pairs when
TCP binding is denied. A built C++ test binary also supplies real handler responses
for schema validation without a listener. The web suite compares core TypeScript
wire types with `docs/openapi.yaml`; JSON syntax in that YAML file avoids another
npm dependency.

The contract command checks every API GET, CSV and replay mirror, previews and
places a far-from-market limit, then cancels it in an isolated simulated replay.
It refuses an existing replay and requires durable journals to check archived
routes. It leaves the completed run in history. Use a local test instance and a
token with `read,replay` or `admin`. Docker CI runs it after the smoke test, with
`pip install jsonschema pyyaml`. Static assets and WebSocket transport have their
own C++ and web tests.

## Pull requests

- Keep each change to one purpose, with tests for the behaviour it adds or fixes.
  The simulator is a deterministic reducer: test it through `TradingSession` with
  explicit market times rather than the wall clock.
- Update the documentation in `docs/` and the README when behaviour, an API field or
  a rule changes.
- Match the surrounding code: its naming, comment density and idioms.
- Commit messages start with the area (`Settlement:`, `Terminal:`, `Cboe:`), then say
  what changed and why.

## Data

To add a market-data feed, see [writing a provider adapter](docs/providers.md) for
the event contract, offline tests and pull-request checklist.

Never commit market data. Recordings of a provider's feed, including Cboe's delayed
quotes, are covered by that provider's terms and may not be redistributed. Tests use
generated or hand-written quotes, and the demo market generates its prices.

## Landing page

The GitHub Pages site is plain HTML and CSS in `site/`, with no build step. The
Pages workflow copies `docs/screenshots/` into its artifact; do not commit another
copy of those images. Keep claims and measurements consistent with the README.
Check phone widths, both colour schemes and reduced motion after changes.

In the repository's Pages settings, select GitHub Actions as the source. Pushes
to `main` affecting the site, screenshots or `.github/workflows/pages.yml` then
deploy it, and the workflow can also be run by hand; until Pages is enabled the
workflow skips the deployment with a notice. Link previews use
`site/social-preview.png` (1280×640).

## Licence

Contributions are accepted under the [MIT licence](LICENSE).
