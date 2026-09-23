#!/usr/bin/env bash
# Build an openport release on this machine, without CI:
#
#   tools/release.sh [--skip-tests] [--no-docker] [--allow-dirty] [--publish]
#
# Checks and builds the web terminal and the engine, runs every test, then writes
# to dist/:
#   openport-VERSION-OS-ARCH.tar.gz   this machine's build: bin/, share/openport/web, docs
#   openport-VERSION-linux-ARCH.tar.gz  the Linux build from the Docker image
#   SHA256SUMS, RELEASE_NOTES.md
# and tags the Docker image openport:VERSION after a smoke test. VERSION comes from
# CMakeLists.txt. Nothing leaves the machine unless --publish is given: then it tags
# vVERSION, pushes the tag and creates a draft GitHub release with gh.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

tests=1 docker=1 dirty=0 publish=0
for arg in "$@"; do
  case "$arg" in
    --skip-tests) tests=0 ;;
    --no-docker) docker=0 ;;
    --allow-dirty) dirty=1 ;;
    --publish) publish=1 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "release: unknown option $arg" >&2; exit 2 ;;
  esac
done

say() { printf '\n== %s\n' "$*"; }
need() { command -v "$1" >/dev/null || { echo "release: $1 is required" >&2; exit 1; }; }
need cmake; need npm; need git; need tar
[ "$docker" = 1 ] && need docker
[ "$publish" = 1 ] && need gh
sha() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

version="$(awk '/^  VERSION [0-9.]+$/ { print $2; exit }' CMakeLists.txt)"
[ -n "$version" ] || { echo "release: no VERSION in CMakeLists.txt" >&2; exit 1; }
commit="$(git rev-parse --short HEAD)"
if [ "$dirty" = 0 ] && [ -n "$(git status --porcelain)" ]; then
  echo "release: the working tree has changes; commit them or pass --allow-dirty" >&2
  exit 1
fi
if [ "$publish" = 1 ] && git rev-parse -q --verify "refs/tags/v$version" >/dev/null; then
  echo "release: v$version is already tagged; raise VERSION in CMakeLists.txt first" >&2
  exit 1
fi
os="$(uname -s | tr '[:upper:]' '[:lower:]')"
arch="$(uname -m)"
dist="$root/dist"
rm -rf "$dist"
mkdir -p "$dist"
echo "openport $version ($commit) on $os/$arch"

say "Web terminal"
(
  cd web
  npm ci --no-audit --no-fund
  npx tsc -p tsconfig.json
  [ "$tests" = 1 ] && npx vitest run
  npx vite build
)

say "Engine ($os/$arch)"
build="$root/build-release"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DOPENPORT_BUILD_BENCHMARKS=OFF \
  -DOPENPORT_BUILD_TESTS="$([ "$tests" = 1 ] && echo ON || echo OFF)" >/dev/null
cmake --build "$build" --parallel
[ "$tests" = 1 ] && ctest --test-dir "$build" --output-on-failure --parallel 8

# One archive: the binaries, the web terminal beside them, and the documentation.
package() {  # package NAME SOURCE-PREFIX
  local name="$1" prefix="$2" stage="$dist/stage/$1"
  mkdir -p "$stage"
  cp -R "$prefix/." "$stage/"
  mkdir -p "$stage/docs"
  cp README.md LICENSE "$stage/"
  cp docs/*.md "$stage/docs/"
  tar -C "$dist/stage" -czf "$dist/$name.tar.gz" "$name"
  echo "wrote dist/$name.tar.gz"
}
native="openport-$version-$os-$arch"
cmake --install "$build" --component openport --prefix "$dist/stage/prefix-native" >/dev/null
"$dist/stage/prefix-native/bin/openportd" --version
package "$native" "$dist/stage/prefix-native"

if [ "$docker" = 1 ]; then
  say "Docker image"
  docker build -t "openport:$version" .
  name="openport-release-smoke-$$"
  docker run -d --rm --name "$name" -p 127.0.0.1::8080 "openport:$version" >/dev/null
  trap 'docker stop "$name" >/dev/null 2>&1 || true' EXIT
  port="$(docker port "$name" 8080/tcp | sed -n '1s/.*://p')"
  for _ in $(seq 1 60); do curl -fsS "http://127.0.0.1:$port/api/status" >/dev/null 2>&1 && break; sleep 1; done
  curl -fsS "http://127.0.0.1:$port/api/status" >/dev/null
  page="$(curl -fsS "http://127.0.0.1:$port/")"
  grep -q '<div id="root">' <<<"$page"
  docker stop "$name" >/dev/null
  trap - EXIT
  echo "openport:$version answers /api/status and serves the terminal"

  say "Linux build from the image"
  image_arch="$(docker image inspect --format '{{.Architecture}}' "openport:$version")"
  linux="openport-$version-linux-$image_arch"
  container="$(docker create "openport:$version")"
  mkdir -p "$dist/stage/prefix-linux/bin" "$dist/stage/prefix-linux/share/openport"
  docker cp "$container:/usr/local/bin/openportd" "$dist/stage/prefix-linux/bin/"
  docker cp "$container:/usr/local/bin/openport-probe" "$dist/stage/prefix-linux/bin/"
  docker cp "$container:/usr/share/openport/web" "$dist/stage/prefix-linux/share/openport/"
  docker rm "$container" >/dev/null
  package "$linux" "$dist/stage/prefix-linux"
fi

rm -rf "$dist/stage"
(cd "$dist" && sha *.tar.gz > SHA256SUMS)

previous="$(git describe --tags --abbrev=0 2>/dev/null || true)"
{
  echo "# openport $version"
  echo
  echo "Built from $commit."
  echo
  echo "## Install"
  echo
  echo "- Docker: \`docker build -t openport .\` from this tag, then \`docker run --rm -p 127.0.0.1:8080:8080 -v openport:/var/lib/openport openport\`."
  echo "- Linux archive: needs OpenSSL 3, zlib and zstd (\`apt install libssl3t64 zlib1g libzstd1\` on Ubuntu 24.04). Unpack it and run \`bin/openportd\`."
  echo "- macOS archive: needs Homebrew's \`openssl@3\`, \`zstd\` and \`brotli\`. Unpack it and run \`bin/openportd\`."
  echo
  echo "Then open http://127.0.0.1:8080. \`openportd --help\` lists every option."
  echo
  if [ -n "$previous" ]; then
    echo "## Changes since $previous"
    echo
    git log --no-merges --format='- %s' "$previous..HEAD"
  else
    echo "## Changes"
    echo
    echo "The first release. See the README for what it does."
  fi
} > "$dist/RELEASE_NOTES.md"

say "Done"
ls -l "$dist"
if [ "$publish" = 1 ]; then
  git tag -a "v$version" -m "openport $version"
  git push origin "v$version"
  gh release create "v$version" --draft --title "openport $version" --notes-file "$dist/RELEASE_NOTES.md" \
    "$dist"/*.tar.gz "$dist/SHA256SUMS"
  echo "Created a draft release for v$version; publish it on GitHub when it looks right."
else
  echo "To publish: tools/release.sh --publish (tags v$version and creates a draft GitHub release)."
fi
